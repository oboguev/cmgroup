#ifdef __x86_64__
  #include <arch/x86/include/generated/uapi/asm/unistd_64.h>
#endif

#ifdef __x86_32__
  #include <arch/x86/include/generated/uapi/asm/unistd_32.h>
#endif

// #include <arch/x86/include/generated/uapi/asm/unistd_x32.h>

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <linux/aio_abi.h>

#ifndef SYS_cmgroup_newgroup
  #define SYS_cmgroup_newgroup 			__NR_cmgroup_newgroup
#endif

#ifndef SYS_cmgroup_associate
  #define SYS_cmgroup_associate 		__NR_cmgroup_associate
#endif

#ifndef SYS_cmgroup_dissociate
  #define SYS_cmgroup_dissociate 		__NR_cmgroup_dissociate
#endif

#ifndef SYS_cmgroup_get_concurrency_value
  #define SYS_cmgroup_get_concurrency_value 	__NR_cmgroup_get_concurrency_value
#endif

#ifndef SYS_cmgroup_set_concurrency_value
  #define SYS_cmgroup_set_concurrency_value 	__NR_cmgroup_set_concurrency_value
#endif

#ifndef SYS_io_set_cmgroup
  #define SYS_io_set_cmgroup 	__NR_io_set_cmgroup
#endif

#define CMGROUP_CLOEXEC (1 << 0)

#define EPOLL_CTL_SET_CMGROUP 4

static inline int cmgroup_newgroup(int nthreads, unsigned int nflags)
{
	return syscall(SYS_cmgroup_newgroup, nthreads, nflags);
}

static inline int cmgroup_associate(int fd)
{
	return syscall(SYS_cmgroup_associate, fd);
}

static inline int cmgroup_dissociate(void)
{
	return syscall(SYS_cmgroup_dissociate);
}

static inline int cmgroup_get_concurrency_value(int fd)
{
	return syscall(SYS_cmgroup_get_concurrency_value, fd);
}

static inline int cmgroup_set_concurrency_value(int fd, int nthreads)
{
	return syscall(SYS_cmgroup_set_concurrency_value, fd, nthreads);
}

static inline int io_set_cmgroup(aio_context_t ctx, int fd)
{
	return syscall(SYS_io_set_cmgroup, ctx, fd);
}

