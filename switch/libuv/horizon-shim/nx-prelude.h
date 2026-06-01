/* Force-included prelude for libuv on Switch. */
#ifndef NX_PRELUDE_H_
#define NX_PRELUDE_H_

/* Expose newlib's pthread/sched priority declarations (guarded by these). */
#ifndef _POSIX_PRIORITY_SCHEDULING
#define _POSIX_PRIORITY_SCHEDULING 1
#endif
#ifndef _POSIX_THREAD_PRIORITY_SCHEDULING
#define _POSIX_THREAD_PRIORITY_SCHEDULING 1
#endif

#ifndef SA_RESETHAND
#define SA_RESETHAND 0x80000000
#endif
#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif

/* newlib lacks getrlimit / struct rlimit; provide a minimal definition. */
#ifndef RLIMIT_NOFILE
#define RLIMIT_NOFILE 7
typedef unsigned long rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
#define RLIM_INFINITY (~0UL)
#endif

#ifdef __cplusplus
extern "C" {
#endif
unsigned int if_nametoindex(const char* ifname);
char* if_indextoname(unsigned int ifindex, char* ifname);
struct statfs;
int  statfs(const char* path, struct statfs* buf);
int  getpriority(int which, int who);
int  setpriority(int which, int who, int prio);
int  getrlimit(int resource, struct rlimit* rlim);
#ifdef __cplusplus
}
#endif
#endif
