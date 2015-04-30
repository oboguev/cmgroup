/*
 * include/linux/cmgroup.h
 *
 * Concurrency management groups.
 *
 * Copyright(C) 2014 Sergey Oboguev <oboguev@yahoo.com>
 *
 * This code is licenced under the GPL version 2 or later.
 * For details see linux-kernel-base/COPYING.
 */

#ifndef _LINUX_CMGROUP_H
#define _LINUX_CMGROUP_H

#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/cache.h>
#include <linux/workqueue.h>

#ifdef CONFIG_CMGROUPS_MAJOR_USE
  #define cmgroup_likely(cond)	   likely(cond)
  #define cmgroup_unlikely(cond)   unlikely(cond)
#else
  #define cmgroup_likely(cond)	   unlikely(cond)
  #define cmgroup_unlikely(cond)   likely(cond)
#endif

#ifdef CONFIG_CMGROUPS
/*
 * Requested tasklet action.
 * Executed when cmgroup->available transitions from 0 to 1.
 */
enum cmgroup_action {
	/*
	 * No action.
	 */
	CMGROUP_ACTION_NEUTRAL = 0,

	/*
	 * Wake all tasks on @wq until cmgroup_available() <= 0
	 * or the queue is empty.
	 * Leave as CMGROUP_ACTION_WAKEALL.
	 * Used by cmgroup_wait based code.
	 */
	CMGROUP_ACTION_WAKEALL,

	/*
	 * If if cmgroup_available() is still > 0 when tasklet
	 * receives control,
	 *   - reset to CMGROUP_ACTION_NEUTRAL
	 *   - wake one tasks on @wq
	 * Used (together with CMGROUP_ACTION_NEUTRAL) by EPOLL code.
	 */
	CMGROUP_ACTION_RESET_WAKEONE
};

/*
 * struct cmgroup - concurrency management group
 *
 * Access to the structure is synchronized mostly via @lock and @kref.
 *
 * Write access to @wq, @lock, @attached_aio and @attached_poll is synchronized
 * via @attached_mtx. These fields are also never changed, even by @attached_mtx
 * holder, if tasklet may be active or pending, as indicated by @tasklet_active.
 * To change them, @attached_mtx must be acquired and tasklet locked out with
 * cmgroup_lockout_tasklet().
 *
 * @kref		Reference count
 *
 * @concurrency_value   Concurrency value for the group,
 *			can only be changed while holding the @lock
 *
 * @available		= (@concurrency_value - count of runnable tasks in this cmgroup)
 *
 *			will be <= 0 if the group is full with runnable tasks
 *			will be == 1 if there is a space for one more task
 *			etc.
 *
 *			We use atomic instructions to synchronize access to @available,
 *			rather than locking. This may result in the number of runnable
 *			tasks sometimes slightly exceed the concurrency value
 *			(typically, by up to one extra task ocassionally), however
 *			this is hopefully overall better for performance, compared
 *			to using spinlocks, since incrementing and decrementing
 *			@available happens on every task state transition from
 *			runnable to non-runnable and vice versa (for tasks belonging
 *			to a cmgroup only). Remember, the purpose of cmgroup mechanism
 *			is to keep the number of runnable tasks in the group at or
 *			slightly above the concurrency value, doing this at a minimum
 *			overhead, rather than providing more costly hard-limit
 *			guarantee.
 *
 * @wq			Pointer to the queue of tasks waiting for cmgroup constraint
 *			to clear, or an event to occur.
 *
 *			For "simple FIFO" case (where tasks are executing a mostly
 *			unmodified event-fetching syscall and then are just waiting
 *			at the very exit from the syscall for cmgroup condition to
 *			clear, in FIFO order) points to @__wq.
 *
 *			For "proper LIFO case" (where cmgroup condition is handled
 *			inside the modified body of an event-fetching syscall,
 *			matching LIFO queue of threads to FIFO queue of events)
 *			points to the queue inside a bound AIO or epoll object.
 *
 * @__wq		Local queue, used in "simple FIFO" case.
 *
 * @lock		Pointer to the lock for @wq,
 *			used also as the lock for cmgroup.
 *
 *			For "proper LIFO" case resides inside a bound AIO or epoll
 *			object; for "simple FIFO" case points to @__qw.lock.
 *
 * @tasklet		Tasklet structure.
 *
 * @tasklet_active      True if tasklet is "in flight":
 *			Set to @true when the code is committed to schedule the tasklet.
 *			(Tasklet is actually scheduled only if the caller was able to
 *			CAS-flip @tasklet_active false->true.)
 *
 *			It is cleared (true->false) after the tasklet finishes processing.
 *
 *			But it can be rasserted by the tasklet again if the tasklet
 *			decides to resume the processing (tasklet resumes only if
 *			it was able to CAS-flip @tasklet_active false->true, otherwise
 *			the tasklet exits since it means other tasklet request was
 *			already spawned).
 *
 * @action              Requested tasklet action.
 *			See description of enum cmgroup_action.
 *
 * @n_waked		Running counter of tasks waked up, changed under @lock,
 *			used by the tasklet to see if any of the pending tasks had
 *			been waked up in the current tasklet wake_up pass.
 *
 * @attached_poll       Pointers to attahed epoll or aio structures, only one can
 * @attached_aio	be non-NULL at a time, or both can be NULL.
 *
 * @attached_mtx	Write access to @attached_poll, @attached_aio @wq and @lock
 *			is synchronized via this mutex
 *
 * @free_work		Work_struct used to deallocate this cmgroup.
 */
struct cmgroup {
	struct kref			kref;
	struct work_struct		free_work;
	enum cmgroup_action		action;

	struct {
		struct mutex		attached_mtx;
		spinlock_t		*lock;
		wait_queue_head_t	*wq;
		int			concurrency_value;
		struct eventpoll	*attached_epoll;
		struct kioctx		*attached_aio;
	} ____cacheline_aligned_in_smp;

	struct {
		wait_queue_head_t	__wq;
	} ____cacheline_aligned_in_smp;

	struct {
		atomic_t		available;
	} ____cacheline_aligned_in_smp;

	struct {
		atomic_t		tasklet_active;
		struct tasklet_struct	tasklet;
		int			n_waked;
	} ____cacheline_aligned_in_smp;
};

void cmgroup_kref_release(struct kref *kref);

static inline void cmgroup_ref(struct cmgroup *cmgroup)
{
	kref_get(&cmgroup->kref);
}

static inline void cmgroup_unref(struct cmgroup *cmgroup)
{
	kref_put(&cmgroup->kref, cmgroup_kref_release);
}

void cmgroup_associate(struct task_struct *task, struct cmgroup *cmgroup, bool addref);
void cmgroup_dissociate(struct task_struct *task);
void __cmgroup_task_enqueued(struct task_struct *task);
void __cmgroup_task_dequeued(struct task_struct *task);

/*
 * Flags ENQUEUE_NOCMGROUP and DEQUEUE_NOCMGROUP suppress cmgroup-related
 * accounting for the number of runnable tasks in the cmgroup.
 * These flags are used when dequeue_task (or deactivate_task) is immediatelly
 * followed by enqueue_task (or activate_task) for the same task, e.g.
 * when moving the task between the runqueues.
 */
#define cmgroup_task_enqueued(task, flags) do {			\
	if (cmgroup_likely((task)->cmgroup != NULL) &&		\
	    !((flags) & ENQUEUE_NOCMGROUP) &&			\
	    !(task)->cmgroup_charged)				\
		__cmgroup_task_enqueued(task);			\
} while (0)
#define cmgroup_task_dequeued(task, flags) do {			\
	if (cmgroup_likely((task)->cmgroup != NULL) &&		\
	    !((flags) & DEQUEUE_NOCMGROUP) &&			\
	    (task)->cmgroup_charged)				\
		__cmgroup_task_dequeued(task);			\
} while (0)

int cmgroup_from_fd(int fd, struct cmgroup **pcmg);
int cmgroup_wait(struct cmgroup *cmgroup, int ret, ktime_t *timeout,
		 ktime_t *remaining, long slack);
void cmgroup_lockout_tasklet(struct cmgroup *cmgroup);
void cmgroup_unlockout_tasklet(struct cmgroup *cmgroup);

/*
 * If returns 0, then optionally call cmgroup_do_attach_aio() and always
 * call cmgroup_end_attach(). If returns error, must not call any of these.
 */
static inline int cmgroup_begin_attach_aio(struct cmgroup *cmgroup,
					   struct kioctx *ioctx)
{
	mutex_lock(&cmgroup->attached_mtx);

	if (unlikely(cmgroup->attached_aio == ioctx)) {
		mutex_unlock(&cmgroup->attached_mtx);
		return -EALREADY;
	}

	if (unlikely(cmgroup->attached_aio != NULL) ||
	    unlikely(cmgroup->attached_epoll != NULL)) {
		mutex_unlock(&cmgroup->attached_mtx);
		return -EBUSY;
	}

	cmgroup_lockout_tasklet(cmgroup);

	return 0;
}

/*
 * If returns 0, then optionally call cmgroup_do_attach_epoll() and always
 * call cmgroup_end_attach(). If returns error, must not call any of these.
 */
static inline int cmgroup_begin_attach_epoll(struct cmgroup *cmgroup,
					     struct eventpoll *epoll)
{
	mutex_lock(&cmgroup->attached_mtx);

	if (unlikely(cmgroup->attached_epoll == epoll)) {
		mutex_unlock(&cmgroup->attached_mtx);
		return -EALREADY;
	}

	if (unlikely(cmgroup->attached_aio != NULL) ||
	    unlikely(cmgroup->attached_epoll != NULL)) {
		mutex_unlock(&cmgroup->attached_mtx);
		return -EBUSY;
	}

	cmgroup_lockout_tasklet(cmgroup);

	return 0;
}

static inline void cmgroup_do_attach_aio(struct cmgroup *cmgroup, struct kioctx *ioctx)
	{ cmgroup->attached_aio = ioctx; }

static inline void cmgroup_do_attach_epoll(struct cmgroup *cmgroup,
					   struct eventpoll *epoll,
					   spinlock_t *lock,
					   wait_queue_head_t *wq)
{
	cmgroup->attached_epoll = epoll;
	cmgroup->lock = lock;
	cmgroup->wq = wq;
}

static inline void cmgroup_end_attach(struct cmgroup *cmgroup)
{
	cmgroup_unlockout_tasklet(cmgroup);
	mutex_unlock(&cmgroup->attached_mtx);
}

void cmgroup_detach(struct cmgroup *cmgroup);

static inline void cmgroup_execute_softirq(void)
{
	/*
	 * Get the tasklet posted by fire_tasklet() be executed, unless we are
	 * in_interrupt(), in which case it will get executed on return from
	 * the interrupt or when softirqs are reenabled.
	 */
	if (unlikely(local_softirq_pending()))
		do_softirq();
}

static inline void cmgroup_request_action(struct cmgroup *cmgroup,
					  enum cmgroup_action action)
	{ cmgroup->action = action; }

/*
 * Read "current" value of cmgroup->available (i.e. remaining number of
 * runnable tasks that can be added without exceeding the cmgroup's
 * concurrency value) as follows:
 *     - mb + compiler barrier before
 *     - read using interlocked instruction
 *     - mb + compiler barrier after
 *
 * Interlocked instruction ensures total inter-CPU ordering between the callers
 * of cmgroup_available() and interlocked updaters of cmgroup->available
 * running concurrently on other CPUs.
 *
 * "Before" and "after" parts of the code that calls cmgroup_available()
 * are locally ordered by the barriers with respect to the call, and will
 * also be seen as globally ordered before/after cmgroup_available() by
 * other interlocked/mb'd accessors of cmgroup->available, and vice versa:
 * e.g. local code executing after cmgroup_available() will see the effects
 * of remote code that executed before the updater.
 *
 * Note that calling cmgroup_available() two or more times in a short
 * sequence (such as before going into a sleep state and after queueing
 * current task into a sleep state, just before schedule()) must be fairly
 * cheap in machines with cache coherence based on MESI or similar
 * protocol, as the cache line with cmgroup->available is likely to be
 * still in the local CPU's cache, so second request will be resolved
 * locally and will not result in an activity between the CPUs.
 */
static __always_inline int cmgroup_available(struct cmgroup *cmgroup)
{
	int val = atomic_read(&cmgroup->available);
	/* implied barrier() + mb() */
	val = atomic_cmpxchg(&cmgroup->available, val, val);
	/* implied barrier() + mb() */
	return val;
}

#else /* ndef CONFIG_CMGROUPS */
struct cmgroup {};
static inline void cmgroup_ref(struct cmgroup *cmgroup) {}
static inline void cmgroup_unref(struct cmgroup *cmgroup) {}
static inline void cmgroup_associate(struct task_struct *task,
				     struct cmgroup *cmgroup) {}
static inline void cmgroup_dissociate(struct task_struct *task) {}
static inline void cmgroup_task_enqueued(struct task_struct *task, int flags) {}
static inline void cmgroup_task_dequeued(struct task_struct *task, int flags) {}
static inline int cmgroup_wait(struct cmgroup *cmgroup, int ret,
			       ktime_t *timeout, ktime_t *remaining,
			       long slack)
	{ return ret; }
static inline void cmgroup_detach(struct cmgroup *cmgroup) {}
static inline void cmgroup_execute_softirq(void) {}
#endif /* CONFIG_CMGROUPS */

#define CMGROUP_CLOEXEC (1 << 0)

#endif
