/*
 * linux/fs/cmgroup.c
 *
 * Concurrency management groups.
 *
 * Copyright(C) 2014 Sergey Oboguev <oboguev@yahoo.com>
 *
 * This code is licenced under the GPL version 2 or later.
 * For details see linux-kernel-base/COPYING.
 */

/*
 *  TODO:
 *
 *  - In the initial implementation, cmgroup control for AIO is implemented as
 *    FIFO wait-queue of tasks at the exit from the event-fetching syscall
 *    (io_getevents).
 *
 *    It would be desirable to restructure it instead as FIFO queue of events
 *    fetched by LIFO queue of tasks (or the latest incoming task, in case the
 *    fetching request can be satisfied immediatelly), with events fetched from
 *    the queue only when wait condition is satisifed because of "nr_events"
 *    or timeout. Before a task is ready to complete the call, events must be
 *    kept in the queue and not bound to any fetching task.
 *
 *    When a task calls io_getevents for AIO context associated with a
 *    concurrency group, calling task wherever possible should have a precedence
 *    for the fetching of pending or incoming events over the tasks that had
 *    been previously waiting on the event queue for this AIO context.
 *
 *    This would reduce the number of context switches and TLB flushes and
 *    maximize cache use for per-task data (such as per-task application-specific
 *    context, user-mode and kernel-mode stacks, kernel data associated with
 *    the task, e.g. task_struct, network structures for the task's possible
 *    connection to the backend server and similar); this would also facilitate
 *    tasks maintaining better affinity to CPUs and reduce cross-CPU migration
 *    for the tasks.
 *
 *    In the streamlined case when a system operates at the limit established
 *    by the cmgroup concurrency value, task calling io_getevents would
 *    immediately pick up the next queued event and continue execution, whereas
 *    previously waiting worker tasks would continue to be blocked. No context
 *    switches would occur, and the running task will be continually picking up
 *    new events, servicing them, then returning to pick up new events again
 *    and so forth, without ideally ever giving up the CPU at the event-fetching
 *    point, whereas blocked tasks would remain stalled, but available as
 *    standby if some active tasks block in the event handlers.
 *
 *    This change however would require a restructuring of AIO code.
 *
 *    (When restructuring it, remember to also do away with herd wakeup
 *    in the current AIO code.)
 *
 *    Described task-LIFO/event-FIFO mode has already been implemented for
 *    epoll code (epoll_wait and epoll_pwait).
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/cmgroup.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/irqflags.h>
#include <linux/seq_file.h>

/* sanity check, and to prevent arithemtic overflows */
#define CMGROUP_MAX_CONCURRENCY_VALUE  (1024 * 1024)


static int cmgroup_fop_release(struct inode *inode, struct file *file);
static void cmgroup_tasklet(unsigned long data);
static bool __cmgroup_wait(struct cmgroup *cmgroup, ktime_t *remaining,
			   long slack_ns);
#ifdef CONFIG_PROC_FS
static int cmgroup_show_fdinfo(struct seq_file *m, struct file *f);
#endif

static const struct file_operations cmgroup_fops = {
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= cmgroup_show_fdinfo,
#endif
	.release	= cmgroup_fop_release,
	.llseek		= noop_llseek,
};

/* get cmgroup for @file */
static inline struct cmgroup *file_cmgroup(struct file *file)
{
	return (struct cmgroup *) file->private_data;
}

static inline bool is_file_cmgroup(struct file *file)
{
	return file->f_op == &cmgroup_fops;
}

/*
 * Call only in tasklet or under the protection of @attached_mtx
 * or when it is otherwise ensured cmgroup->lock cannot be changed.
 */
static __always_inline void cmgroup_lock_irq(struct cmgroup *cmgroup)
	__acquires(cmgroup->lock)
{
	spin_lock_irq(cmgroup->lock);
}

static __always_inline void cmgroup_unlock_irq(struct cmgroup *cmgroup)
	__releases(cmgroup->lock)
{
	spin_unlock_irq(cmgroup->lock);
}

/*
 * Call only in tasklet or under the protection of @attached_mtx
 * or when it is otherwise ensured cmgroup->lock cannot be changed.
 */
#define cmgroup_lock_irqsave(cmgroup, irq_flags) \
	__cmgroup_lock_irqsave((cmgroup), &(irq_flags))

static __always_inline void __cmgroup_lock_irqsave(struct cmgroup *cmgroup,
						   unsigned long *pflags)
	__acquires(cmgroup->lock)
{
	unsigned long flags;
	spin_lock_irqsave(cmgroup->lock, flags);
	*pflags = flags;
}

static __always_inline void cmgroup_unlock_irqrestore(struct cmgroup *cmgroup,
						      unsigned long flags)
	__releases(cmgroup->lock)
{
	spin_unlock_irqrestore(cmgroup->lock, flags);
}

/* allocate and initiaize cmgroup object */
static int cmgroup_alloc(struct cmgroup **pcmgroup, int nthreads)
{
	struct cmgroup *cmgroup = kmalloc(sizeof(*cmgroup), GFP_KERNEL);
	if (unlikely(cmgroup == NULL))
		return -ENOMEM;

	kref_init(&cmgroup->kref);
	cmgroup->concurrency_value = nthreads;
	atomic_set(&cmgroup->available, nthreads);

	cmgroup->wq = &cmgroup->__wq;
	cmgroup->lock = &cmgroup->__wq.lock;
	init_waitqueue_head(cmgroup->wq);
	cmgroup->action = CMGROUP_ACTION_WAKEALL;

	tasklet_init(&cmgroup->tasklet, cmgroup_tasklet, (unsigned long) cmgroup);
	atomic_set(&cmgroup->tasklet_active, false);

	cmgroup->n_waked = 0;
	cmgroup->attached_epoll = NULL;
	cmgroup->attached_aio = NULL;

	mutex_init(&cmgroup->attached_mtx);

	*pcmgroup = cmgroup;
	return 0;
}

static inline void __cmgroup_free(struct cmgroup *cmgroup)
{
	mutex_destroy(&cmgroup->attached_mtx);
	kfree(cmgroup);
}

static void __cmgroup_free_work(struct work_struct *work)
{
	struct cmgroup *cmgroup = container_of(work, struct cmgroup, free_work);
	__cmgroup_free(cmgroup);
}

static inline void cmgroup_free(struct cmgroup *cmgroup)
{
	if (in_interrupt() || irqs_disabled()) {
		INIT_WORK(&cmgroup->free_work, __cmgroup_free_work);
		schedule_work(&cmgroup->free_work);
	} else {
		__cmgroup_free(cmgroup);
	}
}

void cmgroup_kref_release(struct kref *kref)
{
	struct cmgroup *cmgroup = container_of(kref, struct cmgroup, kref);
	cmgroup_free(cmgroup);
}

static int cmgroup_fop_release(struct inode *inode, struct file *file)
{
	struct cmgroup *cmgroup = file_cmgroup(file);

	if (cmgroup)
		cmgroup_unref(cmgroup);

	return 0;
}

/*
 * Get cmgroup for file descriptor.
 * Returned cmgroup object's reference count is incremented.
 */
int cmgroup_from_fd(int fd, struct cmgroup **pcmgroup)
{
	struct fd f = fdget(fd);
	struct cmgroup *cmgroup;
	int error = 0;

	if (!f.file)
		return -EBADF;

	if (!is_file_cmgroup(f.file)) {
		error = -EINVAL;
		goto out;
	}

	cmgroup = file_cmgroup(f.file);
	if (cmgroup == NULL) {
		error = -EINVAL;
		goto out;
	}
	cmgroup_ref(cmgroup);
	*pcmgroup = cmgroup;

out:
	fdput(f);
	return error;
}

#ifdef CONFIG_PROC_FS
static int cmgroup_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct cmgroup *cmgroup = file_cmgroup(f);
	int ret;
	char *attached;

	mutex_lock(&cmgroup->attached_mtx);

	if (cmgroup->attached_epoll != NULL)
		attached = "attached to epoll";
	else if (cmgroup->attached_aio != NULL)
		attached = "attached to aio";
	else
		attached = "unattached";

	ret = seq_printf(m, "cmgroup: concurrency value: %d, available: %d, %s\n",
			 cmgroup->concurrency_value,
			 cmgroup_available(cmgroup),
			 attached);

	mutex_unlock(&cmgroup->attached_mtx);

	return ret;
}
#endif

/*
 * Queue a tasklet, unless it had already been queued and is either in pending
 * or executing state.
 *
 * If @in_sched is true, the caller may be holding scheduling locks (such as
 * runqueue spinlock).
 */
static __always_inline void fire_tasklet(struct cmgroup *cmgroup, bool in_sched)
{
	/* issues MB for earlier @available changes */
	if (false == atomic_cmpxchg(&cmgroup->tasklet_active, false, true)) {
		/*
		 * Make extra reference to cmgroup object so the tasklet will
		 * have a safe handle to it, and cmgroup won't be deallocated
		 * while the tasklet is pending or executing.
		 * Created reference will be released by the tasklet.
		 */
		cmgroup_ref(cmgroup);

		if (likely(in_sched)) {
			/*
			 * We are likely to be holding scheduler locks, such as
			 * runqueue spinlock. Therefore we should not be trying
			 * to wake up softirqd (as this would cause an attempted
			 * recursive spinlock acquisition and a lock-up),
			 * but this is exactly what regular call sequence
			 *
			 *   task_schedule ->
			 *       __tasklet_schedule ->
			 *           raise_softirq_irqoff ->
			 *               wakeup_softirqd
			 *
			 * will do, unless we are in_interrupt().
			 */
			tasklet_schedule_nosoftirqd_wake(&cmgroup->tasklet);
		} else {
			tasklet_schedule(&cmgroup->tasklet);
		}
	}
}

/*
 * One more runnable task in cmgroup.
 * Called when:
 *
 *   - task is being enqueued to a run queue,
 *     task->cmgroup != NULL and task->cmgroup_charged == false.
 *
 *   - runnable task is being associated with a cmgroup,
 *     task->cmgroup != NULL and task->cmgroup_charged == false.
 *
 */
static __always_inline void cmgroup_one_more_runnable(struct task_struct *task)
{
	struct cmgroup *cmgroup = task->cmgroup;
	task->cmgroup_charged = true;
	atomic_dec(&cmgroup->available);
}

/*
 * One less runnable task in cmgroup.
 * Called when:
 *
 *    - task is being dequeued from a run queue,
 *      task->cmgroup != NULL and task->cmgroup_charged == true.
 *
 *    - task is being dissociated from a cmgroup,
 *      task->cmgroup != NULL and task->cmgroup_charged == true.
 *
 * If @in_sched is true, the caller may be holding scheduling locks (such as
 * runqueue spinlock).
 */
static __always_inline void cmgroup_one_less_runnable(struct task_struct *task,
						      bool in_sched)
{
	/*
	 * Increment the count of remaining openings for runnable tasks in the
	 * cmgroup. If @available transitions from 0 to 1, fire a tasklet to
	 * make one of the tasks waiting in @wq runnable... unless the tasklet
	 * is already active.
	 *
	 * (As the tasklet is pending and then running, more runnable openings
	 * for cmgroup may appear, in which case the tasklet may unblock more
	 * than one task from @wq.)
	 *
	 * We process only @available transitions from 0 to 1.
	 * Transition to lower values (@available <= 0) means there are no
	 * runnable task openings in the group yet, and tasks pending in @wq
	 * have to wait.
	 * Transition to higher values (@available > 1) means the unblocking
	 * action had already been taken.
	 *
	 * It may be tempting to check whether there are any tasks pending
	 * in @wq before firing a tasklet, by calling waitqueue_active(...),
	 * however we cannot do this properly without taking @wq.lock (since
	 * the check without taking the lock would mean insertions into @wq
	 * can be missed). This would in most cases mean acquiring and releasing
	 * @wq.lock twice: first in this routine, then in the tasklet.
	 * Given expected frequencies of outcomes, it is probably more efficient
	 * on the balance to just always fire the tasket and perform locking
	 * and check just in the tasklet, instead of in the tasklet and here.
	 *
	 * Furthermore, acquiring the lock here would create a possible deadlock
	 * with the tasklet, since the tasklet performs wake_up while holding
	 * @wq.lock and wake_up will attempt to acquire sched-related locks
	 * (such as runqueue lock), but we come here being called from the
	 * scheduler possibly already holding sched-related locks, so attempting
	 * to acquire @wq.lock may cause a deadlock.
	 *
	 * Furthermore, when the tasklet calls wake_up, this invokes the
	 * scheduler, which may result result in this routine being called as
	 * nested, therefore attempting to aquire @wq.lock here may result in
	 * nested spinlock acquisition attempt causing a lock-up.
	 *
	 * The latter issues can be solved by having custom @wqentry.func that
	 * would call a customized version of try_to_wake_up that will perform
	 * a callback right before it is committed to make the task runnable.
	 * The callback could then remove @wqentry off @wq and release @wq.lock
	 * at this point, so actual task wakeup happens when @wq.lock is not
	 * held.
	 *
	 * However for a sum of mentioned reasons it appears a better approach
	 * just not to take @wq.lock here at all.
	 */

	struct cmgroup *cmgroup = task->cmgroup;
	task->cmgroup_charged = false;

	if (atomic_inc_return(&cmgroup->available) == 1)
		fire_tasklet(cmgroup, in_sched);
}

/*
 * Called when task is being enqueued to a run queue,
 * task->cmgroup != NULL and task->cmgroup_charged == false.
 */
void __cmgroup_task_enqueued(struct task_struct *task)
{
	cmgroup_one_more_runnable(task);
}

/*
 * Called when task is being dequeued from a run queue,
 * task->cmgroup != NULL and task->cmgroup_charged == true.
 */
void __cmgroup_task_dequeued(struct task_struct *task)
{
	cmgroup_one_less_runnable(task, true);
}

static void cmgroup_tasklet(unsigned long data)
{
	struct cmgroup *cmgroup = (struct cmgroup *) data;
	unsigned long irq_flags;
	int n_waked_before;
	static const int max_passes = 5;
	int pass = 0;
	bool unref = true;

again:

	cmgroup_lock_irqsave(cmgroup, irq_flags);

	if (unlikely(cmgroup->action == CMGROUP_ACTION_NEUTRAL))
		goto out_tasklet;

	if (cmgroup->action == CMGROUP_ACTION_RESET_WAKEONE) {
		if (likely(cmgroup_available(cmgroup) > 0)) {
			cmgroup->action = CMGROUP_ACTION_NEUTRAL;
			if (likely(waitqueue_active(cmgroup->wq)))
				wake_up_locked(cmgroup->wq);
			goto out_tasklet;
		} else {
			goto out_recheck;
		}
	}

again_wakeall:
	/* cmgroup->action == CMGROUP_ACTION_WAKEALL */

	if (!waitqueue_active(cmgroup->wq))
		goto out_tasklet;

	if (unlikely(++pass > max_passes)) {
		/*
		 * in the very unlikely event of wake retries or repeated
		 * serial cmgroup's task blocking, will after some number
		 * of passes proceed in softirqd context
		 */
		tasklet_schedule(&cmgroup->tasklet);
		unref = false;   /* pass on reference to new tasklet instance */
		goto out_unlock;
	}

	/*
	 * re-check if runnable openings are still available,
	 * they might have gone in time between the tasklet
	 * was scheduled and the time we got here
	 */
	if (cmgroup_available(cmgroup) > 0) {
		/* wake up one pending task */
		n_waked_before = cmgroup->n_waked;
		wake_up_locked(cmgroup->wq);

		/*
		 * Were all tasks on the wait queue un-wakeable?
		 * Can happen if the tasks are already being/been
		 * waken up. There is nothing for us to do but to quit.
		 */
		if (n_waked_before == cmgroup->n_waked)
			goto out_tasklet;
		else
			goto again_wakeall;
	}

out_recheck:

	/* signal: taklet is over (mb before and after) */
	atomic_cmpxchg(&cmgroup->tasklet_active, true, false);
	cmgroup_unlock_irqrestore(cmgroup, irq_flags);

	/* any more runnable openings appeared at the last instant? */
	if (cmgroup_available(cmgroup) <= 0)     /* mb before and after */
		goto out;    /* no, exit the loop */

	/* more runnable openings just came in,
	   try to reassert "tasklet active" status */
	if (false != atomic_cmpxchg(&cmgroup->tasklet_active, false, true))
		goto out;    /* could not: another tasklet already spawned */

	goto again;

out_tasklet:
	/* totally serialized with prior cmgroup_available */
	/* mb here */
	atomic_cmpxchg(&cmgroup->tasklet_active, true, false);
	/* mb here */

out_unlock:
	cmgroup_unlock_irqrestore(cmgroup, irq_flags);

out:
	/* release cmgroup reference passed from __cmgroup_task_dequeued */
	if (likely(unref))
		cmgroup_unref(cmgroup);
}

/*
 * Associate current task with cmgroup.
 * If @addref is true, add reference to the cmgroup object,
 * otherwise reference had already been added.
 */
void cmgroup_associate(struct task_struct *task, struct cmgroup *cmgroup, bool addref)
{
	if (cmgroup == task->cmgroup) {
		if (!addref)
			cmgroup_unref(cmgroup);
	} else {
		if (addref)
			cmgroup_ref(cmgroup);
		if (task->cmgroup)
			cmgroup_dissociate(task);
		task->cmgroup = cmgroup;
		task->cmgroup_charged = false;
		cmgroup_one_more_runnable(task);
	}
}

/*
 * Dissociate current task from its cmgroup.
 */
void cmgroup_dissociate(struct task_struct *task)
{
	struct cmgroup *cmgroup = task->cmgroup;

	if (cmgroup != NULL) {
		if (task->cmgroup_charged == true)
			cmgroup_one_less_runnable(task, false);
		task->cmgroup = NULL;
		cmgroup_unref(cmgroup);
	}
}

struct cmgroup_wait_struct {
	wait_queue_t wait;
	struct cmgroup *cmgroup;
};

static int cmgroup_wake_function(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	int ret = autoremove_wake_function(wait, mode, sync, key);

	if (ret) {
		struct cmgroup_wait_struct *ws =
			container_of(wait, struct cmgroup_wait_struct, wait);
		struct cmgroup *cmgroup = ws->cmgroup;
		cmgroup->n_waked++;
	}

	return ret;
}

/*
 * This routine (cmgroup_wait) is needed only until we implement a proper
 * thread-LIFO/event-FIFO processing in AIO code for cmgroup != NULL case.
 *
 * cmgroup_wait is called at the end of io_getevents if AIO context is
 * associated with cmgroup.
 *
 * @cmgroup	cmgroup object (verified by called to be non-NULL)
 * @ret		syscall return code to this point
 * @timeout	initial timeout requested for the syscall or NULL for infinite timeout
 * @remaining	remaining timeout (or NULL if @timeout is NULL)
 *
 * Return value will become new syscall return code.
 *
 * If ret == 0 (meaning no events fetched and time-outed or zero-timeout call),
 * will return 0 right away, without waiting.
 *
 * If ret is -EINTR or -ERESTARTSYS, will return the original "ret" value right
 * away, without waiting.
 *
 * If ret > 0 (meaning some events were fetched) or ret < 0 (meaning error),
 * will return the original "ret" value but is subject to blocking wait
 * before the return: if there is not enough runnable task openings in the
 * cmgroup, cmgroup_wait will put current task in interruptible wait state
 * until an opening appears or @remaining timeout expires.
 *
 * In the latter case (if timeout expires) return code is *not* forced
 * to -ETIME, since override of cmgroup wait state by timeout does not
 * mean that core syscall timed out, therefore in case of cmgroup
 * timeout we retain the original syscall return status.
 *
 * Likewise, if wait inside cmgroup_wait is interrupted by a signal,
 * cmgroup_wait will *not* reset return code to -EINTR/-ERESTARTSYS, since the
 * core syscall had not been interrupted, only cmgroup constraint was overriden.
 *
 * In any of these two events we preserve the original "ret" value.
 * It is additionally required in ret > 0 case, since otherwise fetched events
 * would be lost. It is also required in ret < 0 case, since otherwise error
 * information would be lost.
 *
 * cmgroup_wait will not attempt to wait if @timeout is specfied as zero
 * (non-blocking call).
 *
 * Caller must ensure that cmgroup object will be kept bound to the same aio
 * (or epoll) object for the duration of the call, in order to keep cmgroup->lock
 * and cmgroup->wq the same throughout the call.
 */
int cmgroup_wait(struct cmgroup *cmgroup, int ret,
		 ktime_t *timeout, ktime_t *remaining, long slack_ns)
{
	if (unlikely(cmgroup->wq != &cmgroup->__wq) ||
	    unlikely(cmgroup->lock != &cmgroup->__wq.lock)) {
		WARN_ONCE(true, "cmgroup_wait with remote queue or lock\n");
		return ret;
	}

	if (unlikely(ret == 0) ||
	    unlikely(ret == -EINTR) ||
	    unlikely(ret == -ERESTARTSYS))
		return ret;

	/* do not impose cmgroup constraint for non-blocking calls */
	if (timeout && timeout->tv64 == 0)
		return ret;

	/* infinite timeout? */
	if (timeout && timeout->tv64 == KTIME_MAX)
		remaining = timeout = NULL;

	/*
	 * run the loop to guard against the wakeups when @available had
	 * gone negative again in the time since the wake_up and before the task
	 * actually resumes execution (because other task in the cmgroup that
	 * was blocked in some non-cmgroup-related wait state became runnable
	 * in the meanwhile)
	 */
	for (;;) {
		if (unlikely(signal_pending(current)))
			break;

		/* fast check: do not block if not constrained */
		if (cmgroup_available(cmgroup) >= 0)
			break;

		if (!__cmgroup_wait(cmgroup, remaining, slack_ns))
			break;
	}

	return ret;
}

/*
 * Return true to continue the wait loop, false to exit the wait.
 *
 * There is a race condition between cmgroup_one_less_runnable + cmgroup_tasklet
 * and __cmgroup_wait. If cmgroup_one_less_runnable detects transition of
 * @available from 0 to 1 and fires a tasklet, and tasklet is waking up
 * a task that was on @wq but was not off run queue at the time, either
 * because of pending signal, or because it did not enter schedule() yet,
 * or schedule() did not remove the task off the run queue yet at the
 * time wake_up was executed, this wake up will not affect the number
 * of runnable threads and @available will stay at 1 instead of going
 * down to 0. However tasklet does takes care of this eventuality by
 * continuing its loop until @available gets down to zero or no
 * wake-able tasks remain in @wq.
 *
 * We can use cmgroup_lock_xxx because the caller ensures cmgroup object will
 * be kept bound to the same aio or epoll object for the duration of the call.
 */
static bool __cmgroup_wait(struct cmgroup *cmgroup, ktime_t *remaining, long slack_ns)
{
	struct task_struct *task;
	struct cmgroup_wait_struct ws;
	unsigned long irq_flags;

	/* no remaining wait interval? */
	if (remaining && remaining->tv64 <= slack_ns)
		return false;

	task = current;
	init_wait(&ws.wait);
	ws.wait.func = cmgroup_wake_function;
	ws.cmgroup = cmgroup;

	cmgroup_lock_irqsave(cmgroup, irq_flags);
	/* redo the check while holding cmgroup->lock to sync with the tasklet */
	if (cmgroup_available(cmgroup) >= 0) {
		cmgroup_unlock_irqrestore(cmgroup, irq_flags);
		return false;
	}
	__add_wait_queue_tail_exclusive(cmgroup->wq, &ws.wait);
	set_current_state(TASK_INTERRUPTIBLE);
	cmgroup_unlock_irqrestore(cmgroup, irq_flags);

	if (unlikely(signal_pending(task))) {
		finish_wait(cmgroup->wq, &ws.wait);
		return false;
	}

	if (remaining) {
		struct hrtimer_sleeper tmr;
		hrtimer_init_on_stack(&tmr.timer, CLOCK_MONOTONIC,
				      HRTIMER_MODE_REL);
		hrtimer_init_sleeper(&tmr, task);
		hrtimer_start_range_ns(&tmr.timer, *remaining,
				       slack_ns,
				       HRTIMER_MODE_REL);
		schedule();
		*remaining = hrtimer_get_remaining(&tmr.timer);
		hrtimer_cancel(&tmr.timer);
		destroy_hrtimer_on_stack(&tmr.timer);
		finish_wait(cmgroup->wq, &ws.wait);
		return remaining->tv64 > slack_ns;
	} else {
		schedule();
		finish_wait(cmgroup->wq, &ws.wait);
		return true;
	}
}

/*
 * Lock out the tasklet, so we can change cmgroup->lock,
 * cmgroup->wq and other fields the tasket references.
 * Caller must keep @attached_mtx.
 */
void cmgroup_lockout_tasklet(struct cmgroup *cmgroup)
{
	/*
	 * Note: msleep < 20ms can sleep for up to 20ms,
	 * see Documentation/timers/timers-howto.txt
	 */
	while (false != atomic_cmpxchg(&cmgroup->tasklet_active, false, true))
		msleep(1);
	/* make sure the tasklet has released the lock on its way out */
	cmgroup_lock_irq(cmgroup);
	cmgroup_unlock_irq(cmgroup);
}

/*
 * Un-lockout the tasklet.
 * Caller must still keep @attached_mtx and release it only afterwards.
 */
void cmgroup_unlockout_tasklet(struct cmgroup *cmgroup)
{
	atomic_cmpxchg(&cmgroup->tasklet_active, true, false);
	/*
	 * Let the tasklet do whatever it may have missed while being locked out
	 */
	fire_tasklet(cmgroup, false);
}

/*
 * EPOLL descriptor or AIO context is being destroyed, detach from it.
 * Called in process context.
 */
void cmgroup_detach(struct cmgroup *cmgroup)
{
	mutex_lock(&cmgroup->attached_mtx);

	/*
	 * Wake up all tasks in the cmgroup's local wait queue
	 * and wait till the queue is empty. Normaly there should
	 * already be no tasks on the queue.
	 *
	 * Note: msleep < 20ms can sleep for up to 20ms,
	 * see Documentation/timers/timers-howto.txt
	 */
	if (cmgroup->wq == &cmgroup->__wq) {
		for (;;) {
			cmgroup_lock_irq(cmgroup);
			if (unlikely(waitqueue_active(cmgroup->wq))) {
				wake_up_all_locked(cmgroup->wq);
				cmgroup_unlock_irq(cmgroup);
				/* give tasks a chance to remove
				   themselves off the queue */
				msleep(1);
			} else {
				cmgroup_unlock_irq(cmgroup);
				break;
			}
		}
	} else {
		cmgroup_lock_irq(cmgroup);
		if (unlikely(waitqueue_active(cmgroup->wq)))
			WARN_ONCE(true, "cmgroup_detach with active remote queue\n");
		cmgroup_unlock_irq(cmgroup);
	}

	cmgroup_lockout_tasklet(cmgroup);

	cmgroup->attached_epoll = NULL;
	cmgroup->attached_aio = NULL;
	cmgroup->wq = &cmgroup->__wq;
	cmgroup->lock = &cmgroup->__wq.lock;
	cmgroup->action = CMGROUP_ACTION_WAKEALL;

	cmgroup_unlockout_tasklet(cmgroup);

	mutex_unlock(&cmgroup->attached_mtx);
	cmgroup_unref(cmgroup);
}

SYSCALL_DEFINE2(cmgroup_newgroup, int, nthreads, unsigned int, flags)
{
	int error, fd;
	struct cmgroup *cmgroup = NULL;
	struct file *file;
	unsigned file_flags = O_RDWR;

	if (flags & ~CMGROUP_CLOEXEC)
		return -EINVAL;

	if (nthreads == 0)
		nthreads = num_active_cpus();
	if (nthreads <= 0 || nthreads > CMGROUP_MAX_CONCURRENCY_VALUE)
		return -EINVAL;

	if (flags & CMGROUP_CLOEXEC)
		file_flags |= O_CLOEXEC;

	/* create the internal data structure */
	error = cmgroup_alloc(&cmgroup, nthreads);
	if (error < 0)
		return error;

	/* create file structure and a free file descriptor */
	fd = get_unused_fd_flags(file_flags);
	if (fd < 0) {
		error = fd;
		goto out_free_cmg;
	}

	file = anon_inode_getfile("[cmgroup]", &cmgroup_fops, cmgroup, file_flags);
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		goto out_free_fd;
	}

	fd_install(fd, file);
	return fd;

out_free_fd:
	put_unused_fd(fd);
out_free_cmg:
	cmgroup_free(cmgroup);
	return error;
}

SYSCALL_DEFINE1(cmgroup_associate, unsigned int, fd)
{
	struct cmgroup *cmgroup = NULL;
	int error = cmgroup_from_fd(fd, &cmgroup);

	if (error == 0)
		cmgroup_associate(current, cmgroup, false);

	return error;
}

SYSCALL_DEFINE0(cmgroup_dissociate)
{
	cmgroup_dissociate(current);
	return 0;
}

SYSCALL_DEFINE1(cmgroup_get_concurrency_value, unsigned int, fd)
{
	struct cmgroup *cmgroup = NULL;
	int error = cmgroup_from_fd(fd, &cmgroup);

	if (error == 0) {
		error = cmgroup->concurrency_value;
		cmgroup_unref(cmgroup);
	}

	return error;
}

SYSCALL_DEFINE2(cmgroup_set_concurrency_value, unsigned int, fd, int, nthreads)
{
	struct cmgroup *cmgroup = NULL;
	int error, change, old, new;

	if (nthreads == 0)
		nthreads = num_active_cpus();
	if (nthreads <= 0 || nthreads > CMGROUP_MAX_CONCURRENCY_VALUE)
		return -EINVAL;

	error = cmgroup_from_fd(fd, &cmgroup);
	if (error == 0) {
		/* acquire mutex so we can use cmgroup_lock_xxx */
		mutex_lock(&cmgroup->attached_mtx);
		cmgroup_lock_irq(cmgroup);

		change = nthreads - cmgroup->concurrency_value;
		cmgroup->concurrency_value = nthreads;
		new = atomic_add_return(change, &cmgroup->available);
		old = new - change;

		/* let the tasklet release appropriate number of waiting tasks */
		if (old < 1 && new >= 1)
			fire_tasklet(cmgroup, false);

		cmgroup_unlock_irq(cmgroup);
		mutex_unlock(&cmgroup->attached_mtx);
		cmgroup_unref(cmgroup);
	}

	return error;
}

