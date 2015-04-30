/*
 * Test utility for cmgroup.
 *
 * Will elevate priority, therefore run it as sudo or using account with
 * chrt privileges.
 *
 *      -t, --threads=NTHREADS     Number of threads to use
 *      -c, --cmgroup              Use cmgroup throttling
 *
 *      -d, --duration=SEC         Test duration (default = 120 seconds)
 *
 *      -e, --event={AIO|EPOLL}    Event source: {AIO|EPOLL}
 *
 *      -m, --msec=MSEC            Work cycle duration (default = 1 ms)
 *      -u, --usec=USEC            Work cycle duration (default = 1000 us)
 *      -l, --loops=NLOOPS         Exact count of worker thread loops,
 *      			   overrides --msec and --usec
 *
 *      -s, --sleep=USEC           Sleep in worker thread (usec, 3 times),
 *      			   e.g. --sleep 10000 => 1/100 sec x 3.
 *      			   Used to rattle run queues to test stability.
 *
 * At completion will print:
 *
 *     Calibrated (loop cycles = xxxxxx), running the test now.
 *     Served events per second: xxxxx
 *     Served eps * handler loopcount: xxxxxx
 *     Average number of active threads: xxx
 *
 * "Loop cycles" is the calibrated size of the handler (to the value specified
 * by --msec or --usec) and can vary somewhat from run to runn due to the
 * variability in calibration.
 *
 * "Served events per second" is average number of events per second handled.
 * Event invokes the handler sized to "loop cycles".
 *
 * "Served eps * handler loopcount" is somewhat more accurate measure of throughput
 * than "Served events per second", compensating for the variability in calibration.
 *
 * "Average number of active threads" counts threads in main userland part of the
 * code. This excludes threads active in kernel mode and in a small portion of
 * userland code as well.
 *
 * To reduce the effects of variability in calibration between test runs,
 * it is advisable to note calibration data printed during the first run
 * and reuse it with --loops option for subsequent runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../common/cmgroup-userlib.h"
#include <sys/types.h>
#include <sched.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <strings.h>
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
#include "../common/aio.h"
// #include <linux/time.h>

#ifndef CLOCK_MONOTONIC_RAW
  #define CLOCK_MONOTONIC_RAW  4
#endif

#ifndef max
  #define max(a, b)  ((a) > (b) ? (a) : (b))
#endif

#define NCELLS (128 * 1024)
#define NSEC_PER_SEC (1000ul * 1000ul * 1000ul)
#define USEC_PER_SEC (1000ul * 1000ul)

#ifndef __O_TMPFILE
  #define __O_TMPFILE	020000000
#endif

#ifndef O_TMPFILE
  #define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#endif

static inline pid_t
gettid(void)
{
    return syscall(__NR_gettid);
}

typedef int bool;

#ifndef true
  #define true 1
#endif

#ifndef false
  #define false 0
#endif

typedef enum __event_mode
{
	MODE_UNDEFINED = 0,
	MODE_AIO,
	MODE_EPOLL
}
event_mode_t;

typedef struct __thread_context
{
	pthread_t thread;
	int thread_index;
	volatile int cells[NCELLS];
	int k1, k2;
}
thread_context_t;

typedef struct __event_info
{
	union {
		int fd;
		struct iocb* iocb;
	};
}
event_info_t;

/*
 * If use_eventfd is true, use eventfd for epoll event source,
 * otherwise use pipes.
 */
const static bool use_eventfd = true;

/* forward declarations */
static error_t parse_opt(int key, char *arg, struct argp_state *state);
static void fatal(const char* msg);
static void fatal_errno(const char* msg, int errcode);
static void fatal_msg(const char* msg);
static void out_of_memory(void);
static thread_context_t* create_thread_context(void);
static void destroy_thread_context(thread_context_t* ctx);
static void calibrate(void);
static void do_work(thread_context_t* ctx);
static void do_crunch(thread_context_t* ctx);
static void* thread_main(void* arg);
static void init_event_pump_aio(void);
static void init_event_pump_epoll(void);
static void post_initial_events_aio(void);
static void post_initial_events_epoll(void);
static void post_event_aio(event_info_t* ev, thread_context_t* ctx);
static void post_event_epoll(event_info_t* ev, thread_context_t* ctx);
static void fetch_event_aio(event_info_t* ev, thread_context_t* ctx);
static void fetch_event_epoll(event_info_t* ev, thread_context_t* ctx);
static void* xmalloc(size_t size);
static void do_usleep(long usec);

/* static data */
static unsigned long n_crunch_cycles;
static bool abort_work = false;
static int nthreads = 0;
static event_mode_t event_mode = MODE_UNDEFINED;
static int worker_sleep_us = 0;
static bool use_cmgroup = false;
static int test_duration = 120;
static double actual_test_duration;
static int cmgroup = -1;
static int crunch_usec = 1000;
static long work_loop_count = 0;

static pthread_mutex_t mtx_inited_threads = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_inited_threads = PTHREAD_COND_INITIALIZER;
static int n_inited_threads = 0;

/* statistics */
static pthread_mutex_t mtx_statistics = PTHREAD_MUTEX_INITIALIZER;
static int n_active = 0;
static int n_runnable = 0;
static unsigned long n_samples = 0;
static double ave_active = 0;
static double ave_runnable = 0;

/* command line parser data */
static char doc[] = "cmgroup test utility";
static char args_doc[] = "";
static struct argp_option options[] = {
       {"threads",  't', "NTHREADS",      0,  "Number of threads" },
       {"event",    'e', "{AIO|EPOLL}",   0,  "Event source: {AIO|EPOLL}" },
       {"sleep",    's', "USEC",          0,  "Sleep in worker thread (usec, 3 times), e.g. --sleep 10000 => 1/100 sec x 3" },
       {"cmgroup",  'c', 0,               0,  "Use cmgroup throttling" },
       {"msec",     'm', "MSEC",          0,  "Work cycle duration (default = 1 ms)" },
       {"usec",     'u', "USEC",          0,  "Work cycle duration (default = 1000 us)" },
       {"loops",    'l', "NLOOPS",        0,  "Explicit work thread loop count, overrides --msec and --usec" },
       {"duration", 'd', "SEC",           0,  "Test duration (default = 120 seconds)" },
       { 0 }
};
static struct argp argp = { options, parse_opt, args_doc, doc };

static inline void init_event_pump(void)
{
	if (event_mode == MODE_AIO)
		init_event_pump_aio();
	else
		init_event_pump_epoll();
}

static inline void post_initial_events(void)
{
	if (event_mode == MODE_AIO)
		post_initial_events_aio();
	else
		post_initial_events_epoll();
}

static inline void post_event(event_info_t* ev, thread_context_t* ctx)
{
	if (event_mode == MODE_AIO)
		post_event_aio(ev, ctx);
	else
		post_event_epoll(ev, ctx);
}

static inline void fetch_event(event_info_t* ev, thread_context_t* ctx)
{
	if (event_mode == MODE_AIO)
		fetch_event_aio(ev, ctx);
	else
		fetch_event_epoll(ev, ctx);
}

int
main(int argc, char** argv)
{
	struct sched_param sc;
	int nt;
	clockid_t clk_id = CLOCK_MONOTONIC_RAW;
	struct timespec t1;
	struct timespec t2;
	int error;

	if (argp_parse(&argp, argc, argv, 0, 0, NULL))
		exit(1);

	/* calibrate crunching loop */
	calibrate();

	/* create cmgroup and init event source */
	if (use_cmgroup) {
		cmgroup = cmgroup_newgroup(0, 0);
		if (cmgroup < 0)
			fatal("cmgroup_newgroup");
	}

	init_event_pump();

	/* create worker threads */
	for (nt = 0;  nt < nthreads;  nt++) {
		thread_context_t* ctx = create_thread_context();
		ctx->thread_index = nt;
		if (error = pthread_create(& ctx->thread, NULL, thread_main, ctx))
			fatal_errno("pthread_create", error);
	}

	/* let the threads initialize */
	pthread_mutex_lock(& mtx_inited_threads);
	if (error = pthread_cond_wait(& cond_inited_threads, & mtx_inited_threads))
		fatal_errno("pthread_cond_wait", error);
	pthread_mutex_unlock(& mtx_inited_threads);

	/* wait a bit to let the things settle down (e.g. threads go into event-fetching syscall) */
	sleep(1);

	/* elevate priority */
	sc.sched_priority = 50;
	if (sched_setscheduler(gettid(), SCHED_RR, &sc))
		fatal("sched_setscheduler (RR)... try sudo");

	if (clock_gettime(clk_id, & t1))
		fatal("clock_gettime");

	/* post events to the queue */
	post_initial_events();

	/* let the event pump self-run for a while */
	do_usleep(USEC_PER_SEC * (long) test_duration);

	/* tell everyone to abort */
	abort_work = true;

	/* print results */
	pthread_mutex_lock(& mtx_statistics);
	if (clock_gettime(clk_id, & t2))
		fatal("clock_gettime");
	actual_test_duration = (t2.tv_sec - t1.tv_sec) * (long) NSEC_PER_SEC + (t2.tv_nsec - t1.tv_nsec);
	actual_test_duration /= (double) NSEC_PER_SEC;
	printf("Served events per second: %g\n", (double) n_samples / actual_test_duration);
	printf("Served eps * handler loopcount: %g\n", (double) n_crunch_cycles * (double) n_samples / actual_test_duration);
	printf("Average number of active threads: %g\n", ave_active);
	if (worker_sleep_us)
		printf("Average number of runnable threads: %g\n", ave_runnable);
	pthread_mutex_unlock(& mtx_statistics);

	exit(0);
}

/* parse a single command line option */
static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
	long l;

	switch (key) {
	case 't':
		if (!isdigit(*arg))
			argp_usage(state);
		l = strtol(arg, NULL, 10);
		if (l <= 0 || l > 16 * 1024)
			argp_usage(state);
		nthreads = (int) l;
		break;

	case 'e':
		if (0 == strcasecmp(arg, "AIO"))
			event_mode = MODE_AIO;
		else if (0 == strcasecmp(arg, "EPOLL"))
			event_mode = MODE_EPOLL;
		else
			argp_usage(state);
		break;

	case 's':
		if (!isdigit(*arg))
			argp_usage(state);
		l = strtol(arg, NULL, 10);
		if (l < 0 || l > 10 * 1000 * 1000)
			argp_usage(state);
		worker_sleep_us = (int) l;
		break;

	case 'c':
		use_cmgroup = true;
		break;

	case 'm':
		if (!isdigit(*arg))
			argp_usage(state);
		l = strtol(arg, NULL, 10);
		if (l <= 0 || l > 60 * 1000)
			argp_usage(state);
		crunch_usec = 1000 * (int) l;
		break;

	case 'u':
		if (!isdigit(*arg))
			argp_usage(state);
		l = strtol(arg, NULL, 10);
		if (l <= 0 || l > 60 * 1000 * 1000)
			argp_usage(state);
		crunch_usec = (int) l;
		break;

	case 'd':
		if (!isdigit(*arg))
			argp_usage(state);
		l = strtol(arg, NULL, 10);
		if (l <= 0 || l > 3600)
			argp_usage(state);
		test_duration = (int) l;
		break;

	case 'l':
		if (!isdigit(*arg))
			argp_usage(state);
		l = strtol(arg, NULL, 10);
		if (l <= 0)
			argp_usage(state);
		work_loop_count = l;
		break;

	case ARGP_KEY_ARG:
		argp_usage(state);
		break;

	case ARGP_KEY_END:
		if (nthreads == 0 || event_mode == MODE_UNDEFINED)
			argp_usage(state);
		break;

        default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static void*
thread_main(void* arg)
{
	thread_context_t* ctx = (thread_context_t*) arg;
	struct sched_param sc;
	event_info_t ev;
	int error;

	/* lower priority */
	sc.sched_priority = 0;
	if (sched_setscheduler(gettid(), SCHED_OTHER, &sc))
		fatal("sched_setscheduler (OTHER)");
	if (setpriority(PRIO_PROCESS, gettid(), 15))
		fatal("setpriority");

	pthread_mutex_lock(& mtx_inited_threads);
	if (++n_inited_threads == nthreads) {
		if (error = pthread_cond_signal(& cond_inited_threads))
			fatal_errno("pthread_cond_signal", error);
	}
	pthread_mutex_unlock(&mtx_inited_threads);

	while (!abort_work) {
		fetch_event(& ev, ctx);
		post_event(& ev, ctx);

		if (abort_work)
			break;

		pthread_mutex_lock(& mtx_statistics);
		if (abort_work) {
			pthread_mutex_unlock(& mtx_statistics);
			break;
		}
		n_samples++;
		n_active++;
		n_runnable++;
		ave_active = (ave_active * (n_samples - 1) + n_active) / n_samples;
		ave_runnable = (ave_runnable * (n_samples - 1) + n_runnable) / n_samples;
		pthread_mutex_unlock(& mtx_statistics);

		do_work(ctx);

		pthread_mutex_lock(& mtx_statistics);
		n_active--;
		n_runnable--;
		pthread_mutex_unlock(& mtx_statistics);
	}

	pthread_exit(NULL);
	return NULL;
}

/* simulate request processing */
static void
do_work(thread_context_t* ctx)
{
	do_crunch(ctx);

	if (worker_sleep_us) {
		/* rattle the run queues */
		pthread_mutex_lock(& mtx_statistics);
		n_runnable--;
		pthread_mutex_unlock(& mtx_statistics);

		do_usleep(worker_sleep_us);
		do_usleep(worker_sleep_us);
		do_usleep(worker_sleep_us);

		pthread_mutex_lock(& mtx_statistics);
		n_runnable++;
		pthread_mutex_unlock(& mtx_statistics);
	}
}

/* perform some computational and memory load */
static void
do_crunch(thread_context_t* ctx)
{
	unsigned long nc;
	unsigned int seed = 1;

	for (nc = 0;  nc < n_crunch_cycles && !abort_work;  nc++) {
		int index = rand_r(& seed);
		ctx->cells[index % NCELLS] = rand_r(& seed);
		ctx->cells[ctx->k1] = ctx->cells[ctx->k2];
		ctx->k1 = (ctx->k1 + 16) % NCELLS;
		ctx->k2 = (ctx->k2 + 16) % NCELLS;
	}
}

static void do_usleep(long usec)
{
	struct timespec ts;
	int rc;

	ts.tv_sec = usec / USEC_PER_SEC;
	ts.tv_nsec = 1000ul * (usec % USEC_PER_SEC);

	do
		rc = nanosleep(&ts, &ts);
	while (rc == -1 && errno == EINTR);
}

/*
 * Calibrate @n_crunch_cycles, so do_crunch(...) takes @crunch_usec.
 *     							   .
 * This is just a rough calibrarion. In practice it will only approximate
 * actual run time, due to effects of cold/warm cache, hyperthreaded/SMT
 * machines and NUMA factors.
 */
static void
calibrate(void)
{
	thread_context_t* ctx = create_thread_context();
	struct sched_param sc;
	clockid_t clk_id = CLOCK_THREAD_CPUTIME_ID;
	struct timespec t1;
	struct timespec t2;
	long dt;
	double fdt;
	int calib_duration;
	long nc;

	/*
	 * calibrate using interval 25% of the main test run time,
	 * or 10 seconds at the minimum
	 */
	calib_duration = max(10, test_duration / 4);

	sc.sched_priority = 50;
	if (sched_setscheduler(gettid(), SCHED_FIFO, &sc))
		fatal("sched_setscheduler (RR)... try sudo");

	if (work_loop_count != 0) {
		sc.sched_priority = 0;
		if (sched_setscheduler(gettid(), SCHED_OTHER, &sc))
			fatal("sched_setscheduler (OTHER)");
		n_crunch_cycles = work_loop_count;
		printf("Pre-calibrated to %lu loop cycles\n", n_crunch_cycles);
		return;
	}

	printf("Calibrating... ");
	fflush(stdout);

	n_crunch_cycles = 1000 * 1000;
	for (;;) {
		if (clock_gettime(clk_id, & t1))
			fatal("clock_gettime");
		do_crunch(ctx);
		if (clock_gettime(clk_id, & t2))
			fatal("clock_gettime");

		dt = (t2.tv_sec - t1.tv_sec) * (long) NSEC_PER_SEC + (t2.tv_nsec - t1.tv_nsec);
		fdt = (double) dt / (double) NSEC_PER_SEC;
		/* get within 5% of 1 second */
		if (fdt >= 0.95 && fdt <= 1.05)
			break;
		n_crunch_cycles = (long) ((double) n_crunch_cycles / fdt);
	}

	/*
	 * Right now, n_crunch_cycles is estimated number of cycles in 1 second.
	 * Run the test for approximately calib_duration seconds.
	 */
	if (clock_gettime(clk_id, & t1))
		fatal("clock_gettime");
	for (nc = 0;  nc < calib_duration;  nc++)
		do_crunch(ctx);
	if (clock_gettime(clk_id, & t2))
		fatal("clock_gettime");

	/*
	 * fdt = actual time (seconds) it took to execute
	 * calib_duration * n_crunch_cycles cycles
	 */
	dt = (t2.tv_sec - t1.tv_sec) * (long) NSEC_PER_SEC + (t2.tv_nsec - t1.tv_nsec);
	fdt = (double) dt / (double) NSEC_PER_SEC;

	/*
	 * number of cycles to take crunch_usec
	 */
	n_crunch_cycles = (long) ((double) crunch_usec * (double) n_crunch_cycles *
				  (double) calib_duration / (fdt * USEC_PER_SEC));

	sc.sched_priority = 0;
	if (sched_setscheduler(gettid(), SCHED_OTHER, &sc))
		fatal("sched_setscheduler (OTHER)");

	destroy_thread_context(ctx);

	printf("\rCalibrated (loop cycles = %lu), running the test now.\n", n_crunch_cycles);
	fflush(stdout);
}

/***************************************************************************
*                         AIO routines     			  	   *
***************************************************************************/

static aio_context_t aio_ctx;
static struct iocb* aio_iocbs;
static struct iocb** aio_iocbs_pp;
static int aio_fd;
static unsigned char aio_buf[4096];

static void
init_event_pump_aio(void)
{
	ssize_t res;
	int nt;
	struct rlimit rlim;

	if (getrlimit(RLIMIT_NOFILE, &rlim))
		fatal("getrlimit RLIMIT_NOFILE");

	if (rlim.rlim_cur < nthreads + 10) {
		rlim.rlim_cur = nthreads + 10;
		if (setrlimit(RLIMIT_NOFILE, &rlim))
			fatal("setrlimit RLIMIT_NOFILE");
	}

	aio_fd = open("/tmp", O_TMPFILE | O_RDWR, S_IRWXU);
	if (aio_fd < 0)
		fatal("open aio temp file");

	res = write(aio_fd, aio_buf, sizeof aio_buf);
	if (res < 0)
		fatal("write aio_fd");
	if (res != sizeof aio_buf)
		fatal_msg("write aio_fd: wrong size");

	aio_iocbs = (struct iocb*) xmalloc(sizeof(struct iocb) * nthreads);

	aio_iocbs_pp = (struct iocb**) xmalloc(sizeof(struct iocb*) * nthreads);
	for (nt = 0;  nt < nthreads;  nt++)
		aio_iocbs_pp[nt] = &aio_iocbs[nt];

	if (io_setup(nthreads + 100, & aio_ctx))
		fatal("io_setup");

	if (use_cmgroup) {
		if (io_set_cmgroup(aio_ctx, cmgroup))
			fatal("io_set_cmgroup");
	}
}

static void
post_initial_events_aio(void)
{
	int res;
	int nt;

	for (nt = 0;  nt < nthreads;  nt++) {
		struct iocb *iocb = &aio_iocbs[nt];
		memset(iocb, 0, sizeof *iocb);
		iocb->aio_fildes = aio_fd;
		iocb->aio_lio_opcode = IOCB_CMD_PREAD;
		iocb->aio_offset = 0;
		iocb->aio_buf = (__u64) aio_buf;
		iocb->aio_nbytes = sizeof(aio_buf);
		iocb->aio_reqprio = 0;
	}

	if (false) {
		/*
		 * one-shot mode is faster, but if it fails,
		 * the reason is not reported by io_submit
		 */
		res = io_submit(aio_ctx, nthreads, aio_iocbs_pp);
		if (res < 0)
			fatal("io_submit");
		if (res != nthreads)
			fatal_msg("io_submit wrong count");
	} else {
		for (nt = 0;  nt < nthreads;  nt++) {
			struct iocb *iocb = &aio_iocbs[nt];
			res = io_submit(aio_ctx, 1, &iocb);
			if (res < 0)
				fatal("io_submit");
			if (res != 1)
				fatal_msg("io_submit wrong count");
		}
	}
}

static void
post_event_aio(event_info_t* ev, thread_context_t* ctx)
{
	struct iocb *iocb = ev->iocb;
	int res;

	memset(iocb, 0, sizeof *iocb);
	iocb->aio_fildes = aio_fd;
	iocb->aio_lio_opcode = IOCB_CMD_PREAD;
	iocb->aio_offset = 0;
	iocb->aio_buf = (__u64) aio_buf;
	iocb->aio_nbytes = sizeof(aio_buf);
	iocb->aio_reqprio = 0;

	res = io_submit(aio_ctx, 1, &iocb);
	if (res < 0)
		fatal("io_submit (resubmit)");
	if (res != 1)
		fatal_msg("io_submit wrong count (resubmit)");
}

static void
fetch_event_aio(event_info_t* ev, thread_context_t* ctx)
{
	int res;
	struct io_event io_event;

	res = io_getevents(aio_ctx, 1, 1, &io_event, NULL);
	if (res < 0)
		fatal("io_getevents");
	if (res != 1)
		fatal_msg("io_getevents: wrong event count");

	ev->iocb = (struct iocb*) io_event.obj;
	if (io_event.res < 0)
		fatal_errno("aio completion", io_event.res);
	if (io_event.res != sizeof(aio_buf)) {
		char msg[200];
		sprintf(msg, "aio completion (wrong byte count: %ld, not %ld)",
			(long) io_event.res, (long) sizeof(aio_buf));
		fatal_msg(msg);
	}
}

/***************************************************************************
*                         EPOLL routines     			  	   *
***************************************************************************/

static int epoll_fd;
static int* pipe_rd_fds;
static int* pipe_wr_fds;
static int* event_fds;

static void
init_event_pump_epoll(void)
{
	struct epoll_event event;
	int nt;
	struct rlimit rlim;
	int nfiles = use_eventfd ? (nthreads + 10)
	                         : (2 * nthreads + 10);

	if (getrlimit(RLIMIT_NOFILE, &rlim))
		fatal("getrlimit RLIMIT_NOFILE");

	if (rlim.rlim_cur < nfiles) {
		rlim.rlim_cur = nfiles;
		if (setrlimit(RLIMIT_NOFILE, &rlim))
			fatal("setrlimit RLIMIT_NOFILE");
	}

	epoll_fd = epoll_create(nthreads);
	if (epoll_fd < 0)
		fatal("epoll_create");

	if (use_eventfd) {
		event_fds = xmalloc(sizeof(int) * nthreads);
		for (nt = 0;  nt < nthreads;  nt++) {
			event_fds[nt] = eventfd(1, 0);
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

	if (use_cmgroup) {
		if (epoll_ctl(epoll_fd, EPOLL_CTL_SET_CMGROUP, cmgroup, NULL))
			fatal("EPOLL_CTL_SET_CMGROUP");
	}
}

static void
post_initial_events_epoll(void)
{
	int nt;

	for (nt = 0;  nt < nthreads;  nt++) {
		if (use_eventfd) {
			/* already non-zero */
		} else {
			char c = 0;
			int nwr = write(pipe_wr_fds[nt], &c, 1);
			if (nwr < 0)
				fatal("pipe write");
			if (nwr != 1)
				fatal_msg("pipe write (unexpected return value)");
		}
	}
}

static void
post_event_epoll(event_info_t* ev, thread_context_t* ctx)
{
	if (use_eventfd) {
		int64_t val = 1;

		int nwr = write(ev->fd, &val, 8);
		if (nwr < 0)
			fatal("eventfd write");
		if (nwr != 8)
			fatal_msg("eventfd write (unexpected return value)");
	} else {
		char c = 0;
		int nwr = write(ev->fd, &c, 1);
		if (nwr < 0)
			fatal("pipe write");
		if (nwr != 1)
			fatal_msg("pipe write (unexpected return value)");
	}
}

static void
fetch_event_epoll(event_info_t* ev, thread_context_t* ctx)
{
       struct epoll_event event;

       int res = epoll_wait(epoll_fd, &event, 1, -1);
       if (res < 0)
	       fatal("poll_wait");
       if (res == 0)
	       fatal_msg("poll_wait no events");
       if (! (event.events & EPOLLIN))
	       fatal_msg("poll_wait not EPOLLIN");

       if (use_eventfd) {
	       int64_t val;

	       ev->fd = event_fds[event.data.u32];

	       res = read(event_fds[event.data.u32], &val, 8);
	       if (res < 0)
		       fatal("eventfd read");
	       if (res != 8)
		       fatal_msg("eventfd read (unexpected return value)");
       } else {
	       char c;
	       ev->fd = pipe_wr_fds[event.data.u32];

	       res = read(pipe_rd_fds[event.data.u32], &c, 1);
	       if (res < 0)
		       fatal("pipe read");
	       if (res != 1)
		       fatal_msg("pipe read (unexpected return value)");
       }
}

/***************************************************************************
*                         Utility routines     			  	   *
***************************************************************************/

static void
fatal_errno(const char* msg, int errcode)
{
	fprintf(stderr, "\nerror: %s: %s\n", msg, strerror(errcode));
	exit(1);
}

static void
fatal(const char* msg)
{
	fatal_errno(msg, errno);
}

static void
fatal_msg(const char* msg)
{
	fprintf(stderr, "\nerror: %s\n", msg);
	exit(1);
}

static void
out_of_memory(void)
{
	fatal_msg("Out of memory");
}

static thread_context_t*
create_thread_context(void)
{
	thread_context_t* ctx = (thread_context_t*) malloc(sizeof(thread_context_t));
	if (ctx == NULL)
		out_of_memory();
	ctx->k1 = 0;
	ctx->k2 = NCELLS / 2;
	return ctx;
}

static void
destroy_thread_context(thread_context_t* ctx)
{
	if (ctx != NULL)
		free(ctx);
}

static void*
xmalloc(size_t size)
{
	void* p = malloc(size);
	if (p == NULL)
		out_of_memory();
	return p;
}

