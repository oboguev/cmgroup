/*
 * Test to ensure some (not all) basic cmgroup calls apear to work
 * and do not crash.
 *
 * To ensure there are no memory leaks, compare /proc/slabinfo before
 * running "t1" and after.
 *
 * Usage:
 *     test1 t1  => create/destroy cmgroup, change concurrency value
 *     test1 t2  => associate cmgroup with thread pool
 *     test1 t3  => test timeout paths
 *     test1 t4  => test cmgroup_wait timeout paths
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include "../common/cmgroup-userlib.h"
#include <sys/types.h>
#include <sched.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <semaphore.h>
#include <ctype.h>
#include <argp.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <aio.h>
#include <linux/aio_abi.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <inttypes.h>
#include <poll.h>
#include "../common/aio.h"

#ifndef __O_TMPFILE
  #define __O_TMPFILE	020000000
#endif

#ifndef O_TMPFILE
  #define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#endif

typedef int bool;

#ifndef true
  #define true 1
#endif

#ifndef false
  #define false 0
#endif

#define NSEC_PER_SEC (1000 * 1000 * 1000)
#define USEC_PER_SEC (1000 * 1000)

#ifndef CLOCK_MONOTONIC_RAW
  #define CLOCK_MONOTONIC_RAW  4
#endif

#define streq(s1, s2)  (0 == strcmp((s1), (s2)))

const static bool debug = false;
const static bool use_cmgroup = true;
const static bool use_aio = true;
const static bool use_epoll = true;
const static bool use_semaphore = false;

/*
 * If use_eventfd is true, use eventfd for epoll event source,
 * otherwise use pipes.
 */
const static bool use_eventfd = true;

static void usage(void);

static void do_test1(void);
static void do_test1_pass(void);

static void do_test2(void);
static void do_test2_common(void);
static void* test2_thread_main(void* arg);

static void do_test3(void);
static void do_test3_common(void);
static void* test3_thread_main(void* arg);

static void do_test4(void);
static void do_test4_common(void);
static void* test4_thread_main(void* arg);

static void send_event(int nt);
static void fetch_event(void);
static bool fetch_event_timeout(int ms);
static void prepare_epoll_test(void);
static void finish_epoll_test(void);
static void prepare_aio_test(void);
static void finish_aio_test(void);
static pthread_t* create_threads(void* (*thread_main)(void* arg));
static void reap_threads(pthread_t* tids);
static void bind_cmgroup(void);
static bool barrier(pthread_barrier_t* barrier);
static void* xmalloc(size_t size);
static void fatal(const char* msg);
static void fatal_msg(const char* msg);
static void fatal_errno(const char* msg, int errcode);
static void out_of_memory(void);
static void reset_rcvd_flags(void);
static void reset_rcvd_flags_locked(void);
static void busy_wait_usec(int wait_usec);
static void system_run(const char* cmd);
static void* system_run_thread_main(void* arg);

#define NTHREADS 100

static int epoll_fd = -1;
static int aio_fd = -1;
static aio_context_t aio_ctx = 0;
static unsigned char aio_buf[4096];
static int* event_fds = NULL;
static int* pipe_rd_fds = NULL;
static int* pipe_wr_fds = NULL;
static bool do_abort = false;
static pthread_barrier_t barrier_nt;
static pthread_barrier_t barrier_ntp1;
static int nthreads = NTHREADS;
static bool* flag_rcvd = NULL;
static pthread_mutex_t mtx_lock = PTHREAD_MUTEX_INITIALIZER;

#define lock() pthread_mutex_lock(& mtx_lock)
#define unlock() pthread_mutex_unlock(& mtx_lock)

static inline pid_t
gettid(void)
{
    return syscall(__NR_gettid);
}

inline static bool check_abort(void)
{
	bool res;
	lock();
	res = do_abort;
	unlock();
	return res;
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		do_test1();
		do_test2();
		do_test3();
		do_test4();
		printf("Completed all tests.\n");
	}
	else if (streq(argv[1], "t1") && argc == 2)
		do_test1();
	else if (streq(argv[1], "t2") && argc == 2)
		do_test2();
	else if (streq(argv[1], "t3") && argc == 2)
		do_test3();
	else if (streq(argv[1], "t4") && argc == 2)
		do_test4();
	else
		usage();

	exit(0);
}

/***************************************************************************
*       			TEST1   				   *
*           create/destroy cmgroup, change concurrency value               *
***************************************************************************/

#define NFDS 100
#define N_IN_PASSES 10000
#define N_EX_PASSES 100

static void do_test1(void)
{
	int pass;
	int k, fd;
	char cmd[256];

	printf("Remember to:\n");
	printf("\n");
	printf("    sudo cat /proc/slabinfo >slabinfo.1\n");
	printf("    [... run the test ...]\n");
	printf("    sudo cat /proc/slabinfo >slabinfo.2\n");
	printf("    meld slabinfo.1 slabinfo.2\n");
	printf("    rm slabinfo.1 slabinfo.2\n");
	printf("\n");

	for (pass = 0;  pass < N_EX_PASSES;  pass++) {
		for (k = 0;  k < N_IN_PASSES;  k++)
			do_test1_pass();
		printf("\rPass %d ...", pass + 1);
		fflush(stdout);
	}

	printf("\r");
	printf("------------------------------\n");

	fd = cmgroup_newgroup(10, 0);
	if (fd < 0)
		fatal("cmgroup_newgroup");
	if (cmgroup_dissociate() < 0)
		fatal("cmgroup_dissociate");
	printf("t1: created cmgroup file:\n");  fflush(stdout);
	sprintf(cmd, "ls -l /proc/%d/fd/%d", getpid(), fd);
	system(cmd);

	printf("\nt1: before associate:\n");  fflush(stdout);
	sprintf(cmd, "cat /proc/%d/fdinfo/%d", getpid(), fd);
	system_run(cmd);

	printf("\nt1: after associate:\n");  fflush(stdout);
	if (cmgroup_associate(fd) < 0)
		fatal("cmgroup_associate");
	sprintf(cmd, "cat /proc/%d/fdinfo/%d", getpid(), fd);
	system_run(cmd);

	printf("\nt1: after dissociate:\n");  fflush(stdout);
	if (cmgroup_dissociate() < 0)
		fatal("cmgroup_dissociate");
	sprintf(cmd, "cat /proc/%d/fdinfo/%d", getpid(), fd);
	system_run(cmd);
	close(fd);

	printf("------------------------------\n");

	printf("Completed test t1.\n");
}

static void do_test1_pass(void)
{
	int fd, fds[NFDS];
	int k, nt;

	for (k = 0;  k < NFDS;  k++) {
		fds[k] = fd = cmgroup_newgroup(10, 0);
		if (fd < 0)
			fatal("cmgroup_newgroup");

		if (cmgroup_associate(fd) < 0)
			fatal("cmgroup_associate");

		if (cmgroup_dissociate() < 0)
			fatal("cmgroup_dissociate");

		if (cmgroup_associate(fd) < 0)
			fatal("cmgroup_associate");

		if (cmgroup_set_concurrency_value(fd, 20) < 0)
			fatal("cmgroup_set_concurrency_value");

		nt = cmgroup_get_concurrency_value(fd);
		if (nt < 0)
			fatal("cmgroup_get_concurrency_value");
		if (nt != 20)
			fatal_msg("cmgroup_get_concurrency_value returned unexpected result (not 20)");

		if (cmgroup_set_concurrency_value(fd, 5) < 0)
			fatal("cmgroup_set_concurrency_value");

		nt = cmgroup_get_concurrency_value(fd);
		if (nt < 0)
			fatal("cmgroup_get_concurrency_value");
		if (nt != 5)
			fatal_msg("cmgroup_get_concurrency_value returned unexpected result (not 5)");
	}

	for (k = 0;  k < NFDS;  k++)
		close(fds[k]);

	if (cmgroup_dissociate() < 0)
		fatal("cmgroup_dissociate");
}

/***************************************************************************
*       			TEST2   				   *
*       	     associate cmgroup with thread pool			   *
***************************************************************************/

static void do_test2(void)
{
	int k;

	for (k = 0;  k < 100;  k++) {
		if (debug)
			printf("Pass %d ...\n", k + 1);
		else
			printf("\rPass %d ...", k + 1);
		fflush(stdout);

		epoll_fd = -1;
		aio_ctx = 0;

		/* cmgroup binding to epoll + thread pool */
		if (use_epoll) {
			prepare_epoll_test();
			if (debug) { printf("Pass %d epoll ...\n", k + 1);  fflush(stdout); }
			do_test2_common();
			if (debug) { printf("EndPass %d epoll ...\n", k + 1);   fflush(stdout); }
			finish_epoll_test();
		}

		/* cmgroup binding to aio + thread pool */
		if (use_aio) {
			prepare_aio_test();
			if (debug) { printf("Pass %d aio ...\n", k + 1);   fflush(stdout); }
			do_test2_common();
			if (debug) { printf("EndPass %d aio ...\n", k + 1);   fflush(stdout); }
			finish_aio_test();
		}
	}

	printf("\r");
	printf("Completed test t2.\n");
}

static void do_test2_common(void)
{
	pthread_t* tids;
	int k, nt;

	do_abort = false;
	tids = create_threads(test2_thread_main);

	/* let the threads enter wait state */
	usleep(10000);

	/* associate the whole pump with cmgroup */
	bind_cmgroup();

	/*
	 * let some time for thread-to-cmgroup binding to complete
	 * (optional, will complete on event fetching anyway)
	 */
	usleep(10000);

	for (k = 0;  k < 10;  k++) {
		if (debug) { printf("SubPass %d ...\n", k + 1);  fflush(stdout); }
		reset_rcvd_flags();
		for (nt = 0;  nt < nthreads;  nt++)
			send_event(nt);
		barrier(&barrier_ntp1);
	}

	if (debug) { printf("Pre-Reaping ...\n");  fflush(stdout); }

	lock();
	reset_rcvd_flags_locked();
	do_abort = true;
	unlock();

	for (nt = 0;  nt < nthreads;  nt++)
		send_event(nt);

	if (debug) { printf("Reaping ...\n");  fflush(stdout); }

	reap_threads(tids);

	if (debug) { printf("Reaped.\n");  fflush(stdout); }
}

static void* test2_thread_main(void* arg)
{
	for (;;) {
		fetch_event();
		if (check_abort())  break;
		barrier(&barrier_ntp1);
		if (check_abort())  break;
	}

	return NULL;
}

/***************************************************************************
*       			TEST3   				   *
*       	          test timeout paths			           *
***************************************************************************/

static void do_test3(void)
{
	int k;

	for (k = 0;  k < 100;  k++) {
		if (debug)
			printf("Pass %d ...\n", k + 1);
		else
			printf("\rPass %d ...", k + 1);
		fflush(stdout);

		epoll_fd = -1;
		aio_ctx = 0;

		/* cmgroup binding to epoll + thread pool */
		if (use_epoll) {
			prepare_epoll_test();
			if (debug) { printf("Pass %d epoll ...\n", k + 1);  fflush(stdout); }
			do_test3_common();
			if (debug) { printf("EndPass %d epoll ...\n", k + 1);   fflush(stdout); }
			finish_epoll_test();
		}

		/* cmgroup binding to aio + thread pool */
		if (use_aio) {
			prepare_aio_test();
			if (debug) { printf("Pass %d aio ...\n", k + 1);   fflush(stdout); }
			do_test3_common();
			if (debug) { printf("EndPass %d aio ...\n", k + 1);   fflush(stdout); }
			finish_aio_test();
		}
	}

	printf("\r");
	printf("Completed test t3.\n");
}

static void do_test3_common(void)
{
	pthread_t* tids;
	int k, nt;

	/* associate with cmgroup */
	bind_cmgroup();

	do_abort = false;
	tids = create_threads(test3_thread_main);

	/* let the threads enter wait state */
	usleep(10000);

	for (k = 0;  k < 2;  k++) {
		reset_rcvd_flags();
		for (nt = 0;  nt < nthreads;  nt++)
			send_event(nt);
		barrier(&barrier_ntp1);
	}

	reset_rcvd_flags();

	reap_threads(tids);
}

static void* test3_thread_main(void* arg)
{
	int k;

	fetch_event();
	barrier(& barrier_ntp1);

	fetch_event();
	barrier(& barrier_ntp1);

	for (k = 0;  k < 3;  k++) {
		if (fetch_event_timeout(100))
			fatal_msg("unexpected event instead of timeout");
	}

	return NULL;
}

/***************************************************************************
*       			TEST4   				   *
*      	           test cmgroup_wait timeout paths			   *
***************************************************************************/

static int test4_timeouts = 0;

static void do_test4(void)
{
	int k;
	int nxcycles = 0;

	for (k = 0;  k < 100;  k++) {
		if (debug)
			printf("Pass %d ...\n", k + 1);
		else
			printf("\rPass %d ...", k + 1);
		fflush(stdout);

		epoll_fd = -1;
		aio_ctx = 0;

		/* cmgroup binding to epoll + thread pool */
		if (false && use_epoll) {
			prepare_epoll_test();
			if (debug) { printf("Pass %d epoll ...\n", k + 1);  fflush(stdout); }
			do_test4_common();
			if (debug) { printf("EndPass %d epoll ...\n", k + 1);   fflush(stdout); }
			finish_epoll_test();
			nxcycles++;
		}

		/* cmgroup binding to aio + thread pool */
		if (use_aio) {
			prepare_aio_test();
			if (debug) { printf("Pass %d aio ...\n", k + 1);   fflush(stdout); }
			do_test4_common();
			if (debug) { printf("EndPass %d aio ...\n", k + 1);   fflush(stdout); }
			finish_aio_test();
			nxcycles++;
		}
	}

	printf("\r");
	printf("Test 4 timeouts: %d (out of %d), some number of timeouts is expected.\n",
	       test4_timeouts,
	       nxcycles * (5 - 1) * nthreads);
	printf("Completed test t4.\n");
}

static void do_test4_common(void)
{
	pthread_t* tids;
	int k, nt;

	/* associate with cmgroup */
	bind_cmgroup();

	do_abort = false;
	tids = create_threads(test4_thread_main);

	/* let the threads enter wait state */
	usleep(10000);

	for (k = 0;  k < 5;  k++) {
		reset_rcvd_flags();
		usleep(5000);
		for (nt = 0;  nt < nthreads;  nt++) {
			send_event(nt);
			// usleep(1000);
		}
		barrier(&barrier_ntp1);
	}

	reset_rcvd_flags();

	reap_threads(tids);
}

static void* test4_thread_main(void* arg)
{
	int k;

	if (setpriority(PRIO_PROCESS, gettid(), 15))
		fatal("setpriority");

	for (k = 0;  k < 5;  k++) {
		if (! fetch_event_timeout(k == 0 ? -1 : 10)) {
			lock(); test4_timeouts++; unlock();
		}
		/*
		 * Take CPU time to cause fetch_event_timeout on
		 * subsequent threads to complete via timeout.
		 *
		 * For syscalls implemented in non-cmgroup-LIFO fashion,
		 * main syscall body produces an event, then the thread blocks
		 * in cmgroup_wait and finally exits from it via a timeout.
		 * This exercises timeout handling code inside cmgroup_wait.
		 */
		busy_wait_usec(20 * 1000);
		barrier(& barrier_ntp1);
	}

	return NULL;
}

/***************************************************************************
*                         Shared routines     			  	   *
***************************************************************************/

static void prepare_epoll_test(void)
{
	struct epoll_event event;
	int nt;

	flag_rcvd = xmalloc(sizeof(bool) * nthreads);
	memset(flag_rcvd, 0, sizeof(bool) * nthreads);

	epoll_fd = epoll_create(nthreads);
	if (epoll_fd < 0)
		fatal("epoll_create");

	if (use_eventfd) {
		event_fds = xmalloc(sizeof(int) * nthreads);
		for (nt = 0;  nt < nthreads;  nt++) {
			event_fds[nt] = eventfd(0, use_semaphore ? EFD_SEMAPHORE : 0);
			if (event_fds[nt] < 0)
				fatal("eventfd");
			memset(&event, 0, sizeof event);
			event.data.u32 = nt;
			event.events = EPOLLIN;
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fds[nt], &event))
				fatal("EPOLL_CTL_ADD");
		}
	} else {
		pipe_rd_fds = xmalloc(sizeof(int) * nthreads);
		pipe_wr_fds = xmalloc(sizeof(int) * nthreads);
		for (nt = 0;  nt < nthreads;  nt++) {
			int pipefd[2];
			if (pipe(pipefd))
				fatal("pipe (create)");
			pipe_rd_fds[nt] = pipefd[0];
			pipe_wr_fds[nt] = pipefd[1];
			/*
			 * read sometimes fails with EWOULDBLOCK on O_NONBLOCK pipe
			 * even after EPOLLIN had been reported
			 */
			// if (fcntl(pipe_rd_fds[nt], F_SETFL, O_NONBLOCK))
			// 	fatal("fctnl");
			memset(& event, 0, sizeof event);
			event.data.u32 = nt;
			event.events = EPOLLIN;
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe_rd_fds[nt], &event))
				fatal("EPOLL_CTL_ADD");
		}
	}

}

static void finish_epoll_test(void)
{
	int nt;

	if (event_fds) {
		for (nt = 0;  nt < nthreads;  nt++)
			close(event_fds[nt]);
		free(event_fds);
		event_fds = NULL;
	}

	if (pipe_rd_fds) {
		for (nt = 0;  nt < nthreads;  nt++) {
			close(pipe_rd_fds[nt]);
			close(pipe_wr_fds[nt]);
		}
		free(pipe_rd_fds);
		free(pipe_wr_fds);
		pipe_rd_fds = NULL;
		pipe_wr_fds = NULL;
	}

	if (close(epoll_fd))
		fatal("close epoll_fd");
	epoll_fd = -1;

	free(flag_rcvd);
	flag_rcvd = NULL;
}

static void prepare_aio_test(void)
{
	int res;

	aio_fd = open("/tmp", O_TMPFILE | O_RDWR, S_IRWXU);
	if (aio_fd < 0)
		fatal("open aio temp file");

	res = write(aio_fd, aio_buf, sizeof aio_buf);
	if (res < 0)
		fatal("write aio_fd");
	if (res != sizeof aio_buf)
		fatal_msg("write aio_fd: wrong size");

	if (io_setup(nthreads + 100, & aio_ctx))
		fatal("io_setup");
}

static void finish_aio_test(void)
{
	if (io_destroy(aio_ctx))
		fatal("io_destroy");
	aio_ctx = 0;

	if (close(aio_fd))
	    fatal("close aio_fd");
	aio_fd = -1;
}

static bool barriers_inited = false;

static pthread_t* create_threads(void* (*thread_main)(void* arg))
{
	pthread_t* tids = (pthread_t*) xmalloc(sizeof(pthread_t) * nthreads);
	int nt;
	int error;

	if (barriers_inited) {
		if (error = pthread_barrier_destroy(& barrier_nt))
			fatal_errno("pthread_barrier_destroy", error);
		if (error = pthread_barrier_destroy(& barrier_ntp1))
			fatal_errno("pthread_barrier_destroy", error);
	}

	if (error = pthread_barrier_init(& barrier_nt, NULL, nthreads))
		fatal_errno("pthread_barrier_init", error);

	if (error = pthread_barrier_init(& barrier_ntp1, NULL, nthreads + 1))
		fatal_errno("pthread_barrier_init", error);

	barriers_inited = true;

	for (nt = 0;  nt < nthreads;  nt++) {
		if (error = pthread_create(& tids[nt], NULL, thread_main, NULL))
			fatal_errno("pthread_create", error);
	}

	return tids;
}

static void reap_threads(pthread_t* tids)
{
	int nt, error;

	for (nt = 0;  nt < nthreads;  nt++) {
		if (error = pthread_join(tids[nt], NULL))
			fatal_errno("pthread_join", error);
	}

	free(tids);

	if (barriers_inited) {
		if (error = pthread_barrier_destroy(& barrier_nt))
			fatal_errno("pthread_barrier_destroy", error);
		if (error = pthread_barrier_destroy(& barrier_ntp1))
			fatal_errno("pthread_barrier_destroy", error);
		barriers_inited = false;
	}
}

static void bind_cmgroup(void)
{
	int cmgroup;

	if (! use_cmgroup)
		return;

	cmgroup = cmgroup_newgroup(2, 0);
	if (cmgroup < 0)
		fatal("cmgroup_newgroup");

	if (epoll_fd >= 0) {
		if (epoll_ctl(epoll_fd, EPOLL_CTL_SET_CMGROUP, cmgroup, NULL))
			fatal("EPOLL_CTL_SET_CMGROUP");
	}

	if (aio_ctx != 0) {
		if (io_set_cmgroup(aio_ctx, cmgroup))
			fatal("io_set_cmgroup");
	}

	if (close(cmgroup))
		fatal("close cmgroup");
}

static void send_event(int nt)
{
	int res;

	if (epoll_fd != -1 && use_eventfd) {
		int64_t val = 1;

		int nwr = write(event_fds[nt], &val, 8);
		if (nwr < 0)
			fatal("eventfd write");
		if (nwr != 8)
			fatal_msg("eventfd write (unexpected return value)");
	}

	if (epoll_fd != -1 && !use_eventfd) {
		char c = 0;

		int nwr = write(pipe_wr_fds[nt], &c, 1);
		if (nwr < 0)
			fatal("pipe write");
		if (nwr != 1)
			fatal_msg("pipe write (unexpected return value)");
	}

	if (aio_ctx != 0) {
		struct iocb iocb;
		struct iocb* piocb = &iocb;
		memset(&iocb, 0, sizeof iocb);
		iocb.aio_fildes = aio_fd;
		iocb.aio_lio_opcode = IOCB_CMD_PREAD;
		iocb.aio_offset = 0;
		iocb.aio_buf = (__u64) aio_buf;
		iocb.aio_nbytes = sizeof(aio_buf);
		iocb.aio_reqprio = 0;
		res = io_submit(aio_ctx, 1, &piocb);
		if (res < 0)
			fatal("io_submit");
		if (res != 1)
			fatal_msg("io_submit wrong count");
	}
}

static bool is_duplicate(unsigned int n)
{
	if (n >= nthreads)
		fatal_msg("Invalid EPOLL notification index");

	/*
	 * check if epoll_wait returned duplicate EPOLLIN notifications
	 * for the same file
	 */
	lock();
	if (flag_rcvd[n]) {
		unlock();
		printf("  >>> Duplicate %u\n", n);  fflush(stdout);
		return true;
	}
	flag_rcvd[n] = true;
	unlock();
	return false;
}

static void fetch_event(void)
{
again:

	if (epoll_fd != -1 && use_eventfd) {
		struct epoll_event event;
		int64_t val;
		int res;

		do {
			res = epoll_wait(epoll_fd, &event, 1, -1);
		} while (res == -1 && errno == EINTR);

		if (res < 0)
			fatal("epoll_wait");
		if (res == 0)
			fatal_msg("epoll_wait no events");
		if (! (event.events & EPOLLIN))
			fatal_msg("epoll_wait not EPOLLIN");

		if (is_duplicate(event.data.u32))
			goto again;

		res = read(event_fds[event.data.u32], &val, 8);
		if (res < 0)
			fatal("eventfd read");
		if (res != 8)
			fatal_msg("eventfd read (unexpected return value size)");
		if (use_semaphore && val != 1)
			fatal_msg("eventfd read (unexpected return semaphore value)");
	}

	if (epoll_fd != -1 && !use_eventfd) {
		struct epoll_event event;
		int res;
		char c;

		do {
			res = epoll_wait(epoll_fd, &event, 1, -1);
		} while (res == -1 && errno == EINTR);

		if (res < 0)
			fatal("epoll_wait");
		if (res == 0)
			fatal_msg("epoll_wait no events");
		if (! (event.events & EPOLLIN))
			fatal_msg("epoll_wait not EPOLLIN");

		if (is_duplicate(event.data.u32))
			goto again;

		res = read(pipe_rd_fds[event.data.u32], &c, 1);
		if (res < 0)
			fatal("pipe read");
		if (res != 1)
			fatal_msg("pipe read (unexpected return value size)");
	}

	if (aio_ctx != 0) {
		struct io_event io_event;
		int res;

		do {
			res = io_getevents(aio_ctx, 1, 1, &io_event, NULL);
		} while (res == -1 && errno == EINTR);

		if (res < 0)
			fatal("io_getevents");
		if (res != 1)
			fatal_msg("io_getevents: wrong event count");
		if (io_event.res < 0)
			fatal_errno("aio completion", io_event.res);
		if (io_event.res != sizeof(aio_buf)) {
			char msg[200];
			sprintf(msg, "aio completion (wrong byte count: %ld, not %ld)",
				(long) io_event.res, (long) sizeof(aio_buf));
			fatal_msg(msg);
		}
	}
}

static bool fetch_event_timeout(int ms)
{
again:

	if (epoll_fd != -1 && use_eventfd) {
		struct epoll_event event;
		int64_t val;
		int res;

		do {
			res = epoll_wait(epoll_fd, &event, 1, ms);
		} while (res == -1 && errno == EINTR);

		if (res == 0)
			return false;
		if (res < 0)
			fatal("epoll_wait");
		if (! (event.events & EPOLLIN))
			fatal_msg("epoll_wait not EPOLLIN");

		if (is_duplicate(event.data.u32))
			goto again;

		res = read(event_fds[event.data.u32], &val, 8);
		if (res < 0)
			fatal("eventfd read");
		if (res != 8)
			fatal_msg("eventfd read (unexpected return value size)");
		if (use_semaphore && val != 1)
			fatal_msg("eventfd read (unexpected return semaphore value)");
	}

	if (epoll_fd != -1 && !use_eventfd) {
		struct epoll_event event;
		char c;
		int res;

		do {
			res = epoll_wait(epoll_fd, &event, 1, ms);
		} while (res == -1 && errno == EINTR);

		if (res == 0)
			return false;
		if (res < 0)
			fatal("epoll_wait");
		if (! (event.events & EPOLLIN))
			fatal_msg("epoll_wait not EPOLLIN");

		if (is_duplicate(event.data.u32))
			goto again;

		res = read(pipe_rd_fds[event.data.u32], &c, 1);
		if (res < 0)
			fatal("pipe read");
		if (res != 1)
			fatal_msg("pipe read (unexpected return value size)");
	}

	if (aio_ctx != 0) {
		struct io_event io_event;
		int res;
		struct timespec ts;

		ts.tv_sec = 0;
		ts.tv_nsec = ms * 1000;

		do {
			res = io_getevents(aio_ctx, 1, 1, &io_event, & ts);
		} while (res == -1 && errno == EINTR);

		if (res == 0)
			return false;
		if (res < 0)
			fatal("io_getevents");
		if (res != 1)
			fatal_msg("io_getevents: wrong event count");
		if (io_event.res < 0)
			fatal_errno("aio completion", io_event.res);
		if (io_event.res != sizeof(aio_buf)) {
			char msg[200];
			sprintf(msg, "aio completion (wrong byte count: %ld, not %ld)",
				(long) io_event.res, (long) sizeof(aio_buf));
			fatal_msg(msg);
		}
	}

	return true;
}

static bool barrier(pthread_barrier_t* barrier)
{
	int error = pthread_barrier_wait(barrier);

	if (error == PTHREAD_BARRIER_SERIAL_THREAD)
		return true;

	if (error != 0)
		fatal_errno("pthread_barrier_wait", error);

	return false;
}

static void reset_rcvd_flags(void)
{
	lock();
	reset_rcvd_flags_locked();
	unlock();
}

static void reset_rcvd_flags_locked(void)
{
	if (flag_rcvd)
		memset(flag_rcvd, 0, nthreads * sizeof(bool));
}

static void busy_wait_usec(int wait_usec)
{
	clockid_t clk_id = CLOCK_MONOTONIC_RAW;
	struct timespec t1;
	struct timespec t2;
	long dt;

	if (clock_gettime(clk_id, & t1))
		fatal("clock_gettime");

	for (;;) {
		if (clock_gettime(clk_id, & t2))
			fatal("clock_gettime");
		dt = (t2.tv_sec - t1.tv_sec) * (long) NSEC_PER_SEC + (t2.tv_nsec - t1.tv_nsec);
		if (dt >= wait_usec * 1000)
			break;
	}
}

static volatile bool system_run_wait;

static void system_run(const char* cmd)
{
	pthread_t tid;
	int error;

	system_run_wait = true;

	if (error = pthread_create(& tid, NULL, system_run_thread_main, (void*) cmd))
		fatal_errno("pthread_create", error);

	while (system_run_wait)
		;

	if (error = pthread_join(tid, NULL))
		fatal_errno("pthread_join", error);
}

static void* system_run_thread_main(void* arg)
{
	const char* cmd = (const char*) arg;
	system(cmd);
	system_run_wait = false;
}

/***************************************************************************
*                         Utility routines     			  	   *
***************************************************************************/

static void usage(void)
{
	fprintf(stderr, "usage: test1 [t1 | t2 | t3]\n");
	exit(1);
}

static void fatal_msg(const char* msg)
{
	fprintf(stderr, "\nerror: %s\n", msg);
	exit(1);
}

static void fatal_errno(const char* msg, int errcode)
{
	fprintf(stderr, "\nerror: %s: %s\n", msg, strerror(errcode));
	exit(1);
}

static void fatal(const char* msg)
{
	fatal_errno(msg, errno);
}

static void out_of_memory(void)
{
	fatal_msg("Out of memory");
}

static void* xmalloc(size_t size)
{
	void* p = malloc(size);
	if (p == NULL)
		out_of_memory();
	return p;
}

