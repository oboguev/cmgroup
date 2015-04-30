static inline int io_getevents(aio_context_t ctx, long min_nr, long nr,
			       struct io_event *events, struct timespec *timeout)
{
	return syscall(__NR_io_getevents, ctx, min_nr, nr, events, timeout);
}

static inline int io_setup(unsigned nr_reqs, aio_context_t *pctx)
{
	return syscall(__NR_io_setup, nr_reqs, pctx);
}

static inline int io_submit(aio_context_t ctx, long nr, struct iocb **iocbpp)
{
	return syscall(__NR_io_submit, ctx, nr, iocbpp);
}

static inline int io_destroy(aio_context_t ctx)
{
	return syscall(__NR_io_destroy, ctx);
}
