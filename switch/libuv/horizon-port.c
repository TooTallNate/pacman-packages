/* Horizon (Nintendo Switch) libc support object for libuv.
 *
 * Provides symbols libuv references that newlib/libnx declare but do not
 * implement on Horizon. Two kinds:
 *   - Implemented for real on top of lseek/read/write: pread/pwrite/readv/writev.
 *   - Stubbed (Switch has no fork/exec, POSIX signals, ownership, TTYs, thread
 *     naming, process priority, or interface enumeration): these make the
 *     relevant uv_* APIs fail cleanly / return defaults instead of failing to
 *     link.
 *
 * This translation unit is intentionally SELF-CONTAINED: it includes every
 * header it needs and locally defines the few types newlib lacks (struct
 * rlimit, struct statfs). It does NOT rely on the force-included nx-prelude.h
 * (that prelude exists for libuv's own sources), so it compiles with a bare
 * `gcc -c horizon-port.c` given only the newlib/libnx include paths.
 */

/* Expose newlib's pthread/sched priority declarations (pthread_t, struct
 * sched_param, pthread_get/setschedparam) which are gated behind these. Must
 * precede <pthread.h>/<sched.h>. */
#ifndef _POSIX_PRIORITY_SCHEDULING
#define _POSIX_PRIORITY_SCHEDULING 1
#endif
#ifndef _POSIX_THREAD_PRIORITY_SCHEDULING
#define _POSIX_THREAD_PRIORITY_SCHEDULING 1
#endif

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/_iovec.h>  /* libnx: struct iovec (newlib has no <sys/uio.h>) */
#include <signal.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <sched.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* newlib's <sys/resource.h> declares getrusage + struct rusage but NOT
 * getrlimit / struct rlimit. Pull it in for rusage, then supply rlimit. */
#include <sys/resource.h>
#ifndef RLIMIT_NOFILE
#define RLIMIT_NOFILE 7
typedef unsigned long rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
#define RLIM_INFINITY (~0UL)
#endif

/* struct statfs is not provided by newlib; the body only uses it by pointer. */
struct statfs;

/* ---- Network interface name<->index + enumeration (unavailable on Switch). */
struct ifaddrs;
unsigned int if_nametoindex(const char* ifname) { (void)ifname; return 0; }
char* if_indextoname(unsigned int ifindex, char* ifname) { (void)ifindex; (void)ifname; return 0; }
int getifaddrs(struct ifaddrs** ifap) { if (ifap) *ifap = 0; errno = ENOSYS; return -1; }
void freeifaddrs(struct ifaddrs* ifa) { (void)ifa; }

/* ---- Filesystem / process-priority queries Horizon doesn't expose. */
int statfs(const char* path, struct statfs* buf) { (void)path; (void)buf; errno = ENOSYS; return -1; }
int getpriority(int which, int who) { (void)which; (void)who; errno = ENOSYS; return 0; }
int setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; errno = ENOSYS; return -1; }

int getrlimit(int resource, struct rlimit* rlim) {
  (void)resource;
  if (rlim) { rlim->rlim_cur = RLIM_INFINITY; rlim->rlim_max = RLIM_INFINITY; }
  return 0;
}

/* ---- POSIX positional / vectored I/O newlib lacks. Implemented for real on
 * top of lseek/read/write so libuv's fs read/write paths actually work. */
ssize_t pread(int fd, void* buf, size_t n, off_t off) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur == (off_t)-1) return -1;
  if (lseek(fd, off, SEEK_SET) == (off_t)-1) return -1;
  ssize_t r = read(fd, buf, n);
  int e = errno;
  lseek(fd, cur, SEEK_SET);
  errno = e;
  return r;
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur == (off_t)-1) return -1;
  if (lseek(fd, off, SEEK_SET) == (off_t)-1) return -1;
  ssize_t r = write(fd, buf, n);
  int e = errno;
  lseek(fd, cur, SEEK_SET);
  errno = e;
  return r;
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < iov[i].iov_len) break;  /* short read */
  }
  return total;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (r < 0) return total ? total : -1;
    total += r;
    if ((size_t)r < iov[i].iov_len) break;  /* short write */
  }
  return total;
}

/* ---- Ownership / timestamps / TTY (no-ops; Switch has a single principal and
 * no TTY). */
int fchown(int fd, uid_t o, gid_t g) { (void)fd;(void)o;(void)g; return 0; }
int chown(const char* p, uid_t o, gid_t g) { (void)p;(void)o;(void)g; return 0; }
int lchown(const char* p, uid_t o, gid_t g) { (void)p;(void)o;(void)g; return 0; }
int futimens(int fd, const struct timespec times[2]) { (void)fd;(void)times; return 0; }
int ttyname_r(int fd, char* buf, size_t len) { (void)fd;(void)buf;(void)len; return ENOTTY; }

/* ---- Signals (no POSIX signal delivery on Horizon). */
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) { (void)how;(void)set; if(old) memset(old,0,sizeof(*old)); return 0; }
int sigprocmask(int how, const sigset_t* set, sigset_t* old) { (void)how;(void)set; if(old) memset(old,0,sizeof(*old)); return 0; }
int sigaction(int sig, const struct sigaction* act, struct sigaction* old) { (void)sig;(void)act; if(old) memset(old,0,sizeof(*old)); return 0; }

int getrusage(int who, struct rusage* usage) { (void)who; if(usage) memset(usage,0,sizeof(*usage)); return 0; }

/* ---- Credentials (single principal). */
uid_t geteuid(void) { return 0; }
pid_t getppid(void) { return 0; }
int setgid(gid_t g) { (void)g; return 0; }
int setuid(uid_t u) { (void)u; return 0; }
int setsid(void) { return 0; }
int setgroups(int n, const gid_t* list) { (void)n;(void)list; return 0; }

int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
  (void)uid;(void)pwd;(void)buf;(void)buflen; if(result) *result = 0; return 0;
}
int getgrgid_r(gid_t gid, struct group* grp, char* buf, size_t buflen, struct group** result) {
  (void)gid;(void)grp;(void)buf;(void)buflen; if(result) *result = 0; return 0;
}

/* ---- Process spawning (no fork/exec on Horizon). */
pid_t waitpid(pid_t pid, int* status, int options) { (void)pid;(void)options; if(status)*status=0; errno = ECHILD; return -1; }
int execvp(const char* file, char* const argv[]) { (void)file;(void)argv; errno = ENOSYS; return -1; }

/* ---- pipe()/pipe2(): Horizon (libnx) has no anonymous pipes and no
 * socketpair(), but it DOES have working loopback TCP. libuv only needs a pipe
 * for its self-wakeup mechanism (the async/signal watcher): one fd it can
 * poll() for readability and another it can write a byte to. Emulate that with
 * a connected 127.0.0.1 TCP socket pair (listen -> connect -> accept). fds[0]
 * is the read end (accepted side), fds[1] the write end (connected side). */
static int nx__loopback_pair(int fds[2]) {
  int lst = -1, cli = -1, srv = -1;
  struct sockaddr_in addr;
  socklen_t alen;

  lst = socket(AF_INET, SOCK_STREAM, 0);
  if (lst < 0) goto fail;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = 0;                              /* ephemeral port */
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* 127.0.0.1 */
  if (bind(lst, (struct sockaddr*)&addr, sizeof(addr)) < 0) goto fail;
  if (listen(lst, 1) < 0) goto fail;

  /* discover the bound port */
  alen = sizeof(addr);
  if (getsockname(lst, (struct sockaddr*)&addr, &alen) < 0) goto fail;

  cli = socket(AF_INET, SOCK_STREAM, 0);
  if (cli < 0) goto fail;
  if (connect(cli, (struct sockaddr*)&addr, sizeof(addr)) < 0) goto fail;

  alen = sizeof(addr);
  srv = accept(lst, (struct sockaddr*)&addr, &alen);
  if (srv < 0) goto fail;

  close(lst);
  fds[0] = srv;  /* read end  */
  fds[1] = cli;  /* write end */
  return 0;

fail: {
    int e = errno;
    if (lst >= 0) close(lst);
    if (cli >= 0) close(cli);
    if (srv >= 0) close(srv);
    errno = e ? e : ENOSYS;
    return -1;
  }
}

int pipe(int fds[2]) {
  if (!fds) { errno = EFAULT; return -1; }
  return nx__loopback_pair(fds);
}

int pipe2(int fds[2], int flags) {
  if (pipe(fds)) return -1;
  if (flags & O_NONBLOCK) {
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
  }
  if (flags & O_CLOEXEC) {
    fcntl(fds[0], F_SETFD, fcntl(fds[0], F_GETFD, 0) | FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, fcntl(fds[1], F_GETFD, 0) | FD_CLOEXEC);
  }
  return 0;
}

/* ---- Thread scheduling / naming (best-effort no-ops). */
int pthread_getschedparam(pthread_t t, int* policy, struct sched_param* param) {
  (void)t; if(policy)*policy=0; if(param) memset(param,0,sizeof(*param)); return 0;
}
int pthread_setschedparam(pthread_t t, int policy, const struct sched_param* param) {
  (void)t;(void)policy;(void)param; return 0;
}
int sched_get_priority_min(int policy) { (void)policy; return 0; }
int sched_get_priority_max(int policy) { (void)policy; return 0; }
int pthread_setname_np(pthread_t t, const char* n) { (void)t;(void)n; return 0; }
int pthread_getname_np(pthread_t t, char* n, size_t len) { (void)t; if(n&&len)n[0]=0; return 0; }

/* ---- sysconf / getpagesize. */
long sysconf(int name) {
  switch (name) {
    case _SC_PAGESIZE:          return 0x1000;
    case _SC_NPROCESSORS_CONF:  return 4;
    case _SC_NPROCESSORS_ONLN:  return 4;
    default:                    return -1;
  }
}
int getpagesize(void) { return 0x1000; }
