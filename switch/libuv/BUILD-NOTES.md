# switch-libuv build notes

libuv **v1.52.1** ported to Nintendo Switch (Horizon / devkitA64 + libnx).

Built with the **stock devkitPro GCC toolchain** (`/opt/devkitpro/cmake/Switch.cmake`)
— no external clang required. The library uses libuv's generic POSIX event
backend (`src/unix/posix-poll.c`) over libnx's BSD sockets + `poll()`.

## What the port consists of

### 1. Source patch — `0001-horizon-switch-port.patch`
Applied to the upstream `dist.libuv.org` tarball. Eight changes:

- **`CMakeLists.txt`**: a `Horizon`/`NintendoSwitch` `CMAKE_SYSTEM_NAME` branch
  selecting the source set:
  `horizon.c posix-hrtime.c posix-poll.c bsd-ifaddrs.c no-proctitle.c no-fsevents.c`.
- **`include/uv/unix.h`**:
  - route `__SWITCH__` to `#include "uv/posix.h"` (gives the loop the
    `poll_fds*` fields the posix-poll backend needs);
  - define `struct ipv6_mreq` right after `<netinet/in.h>` (libnx defines
    `IPV6_JOIN_GROUP`/`struct in6_addr` but omits `ipv6_mreq`).
- **`src/unix/core.c`**: two changes.
  - Guard the extended `getrusage` field copies (`ru_maxrss`, `ru_minflt`, …)
    with `!defined(__SWITCH__)` — newlib's `struct rusage` only has
    `ru_utime`/`ru_stime`. They stay zero in `uv_rusage_t`, which is honest
    (Switch doesn't track them).
  - Make `uv__cloexec()` a no-op on `__SWITCH__`. Horizon has no `exec()`, so
    `FD_CLOEXEC` is meaningless, AND libnx's `fcntl()` only supports
    `F_GETFL`/`F_SETFL` — for any other command (like `F_SETFD`) it returns
    `EOPNOTSUPP` as a **positive** value rather than `-1`+errno. The stock
    `uv__cloexec` treats that non-zero return as failure and reports a spurious
    error with a stale errno, which makes `uv__accept()` **close every freshly
    accepted socket**. Symptom: a TCP server accepts exactly one connection and
    then silently drops all subsequent ones. (Found via on-hardware TCP
    benchmarking; see Hardware validation.)
- **`src/unix/signal.c`** and **`src/threadpool.c`**: skip the two
  `pthread_atfork()` registrations under `__SWITCH__` (in
  `uv__signal_global_init` and the thread-pool `init_once`). Horizon has no
  `fork()`; newlib's `pthread_atfork` touches fork/reentrancy state that faults
  under hbloader. These were the cause of hard crashes inside
  `uv_default_loop()` (signal global init) and the first `uv_queue_work()`
  (thread-pool init) on real hardware.
- **`src/threadpool.c`** (pkgrel 3): honor a new `UV_THREADPOOL_STACK_SIZE`
  env var (bytes) in `init_threads`, complementing upstream's
  `UV_THREADPOOL_SIZE`. Upstream hardcodes 8 MiB per worker stack; with the
  default 4 workers that is a 32 MiB commit at the FIRST `uv_queue_work()` —
  which a memory-constrained embedder (Switch applet mode: ~380 MiB process
  grant, ~168 MiB malloc heap) cannot satisfy, and a failed worker create is a
  hard `abort()`. Values are rounded up to page size and clamped to the
  platform minimum by `uv_thread_create_ex`. Embedders should set BOTH env
  vars before the first async op (nx.js sets 2-4 workers × 1 MiB from its
  `nxjs.ini` `[threadpool]` section).
- **`src/uv-common.c`**: disable the `__attribute__((destructor))` on
  `uv_library_shutdown` under `__SWITCH__`. That destructor runs from
  `__libc_fini_array` at process teardown — after a homebrew app has typically
  called `socketExit()` — so closing the socket-backed signal/async pipe fds
  there dereferences torn-down libnx state and faults on exit. Embedders call
  `uv_library_shutdown()` explicitly (while the socket layer is up) instead.
- **`src/unix/horizon.c`** (new): the per-platform "util" file. Memory queries
  via `svcGetInfo` (`Total`/`Used` memory), `uv_uptime` via `armGetSystemTick`
  (19.2 MHz), `uv_cpu_info` reporting the 4 Cortex-A57 cores, `uv_loadavg`/
  `uv_exepath` no-ops. `uv__io_poll` and the loop init/delete come from
  `posix-poll.c`.

### 2. libc support object — `horizon-port.c` → `libuv-horizon-port.o`
Symbols newlib **declares but does not implement** on Horizon. Two kinds:

- **Implemented for real** on top of `lseek`/`read`/`write`:
  `pread`, `pwrite`, `readv`, `writev`.
- **`pipe`/`pipe2`**: implemented via a connected `127.0.0.1` TCP socket pair
  (listen → connect → accept). libnx has no anonymous pipes and its
  `socketpair()` is unimplemented, but loopback TCP works. libuv needs a pipe
  for its async/signal self-wakeup (poll one fd, write a byte to the other);
  the socket pair satisfies that. **This is essential** — without it
  `uv_loop_init` cannot set up the async watcher.
- **Stubbed** (Switch has no fork/exec, POSIX signals, ownership, TTYs,
  thread naming, process priority, interface enumeration) — they make the
  relevant `uv_*` APIs fail cleanly or return defaults rather than fail to
  link: `getrusage`, `sigaction`/`sigprocmask`/`pthread_sigmask`, `waitpid`/
  `execvp`, `geteuid`/`getppid`/`setuid`/`setgid`/`setsid`/`setgroups`,
  `getpwuid_r`/`getgrgid_r`, `chown`/`fchown`/`lchown`/`futimens`,
  `ttyname_r`, `pthread_setname_np`/`getname_np`,
  `pthread_get/setschedparam`/`sched_get_priority_min`/`max`,
  `getpriority`/`setpriority`, `getrlimit`, `statfs`, `sysconf`/`getpagesize`,
  `if_nametoindex`/`if_indextoname`, `getifaddrs`/`freeifaddrs`.

### 3. Header shims — `horizon-shim/`
Headers libuv `#include`s that newlib/libnx don't ship (or ship incomplete).
Embedders must put this dir on `-isystem` **ahead of** newlib and force-include
`nx-prelude.h`:

- `nx-prelude.h` — turns on `_POSIX_PRIORITY_SCHEDULING` /
  `_POSIX_THREAD_PRIORITY_SCHEDULING` (so newlib exposes its pthread/sched
  decls), defines `SA_RESETHAND`, `PRIO_PROCESS`, `struct rlimit`/`RLIMIT_*`,
  and declares the functions the support object provides.
- `sys/termios.h` — full `struct termios`/`winsize`, the `c_iflag`/`c_oflag`/
  `c_cflag`/`c_lflag` bit constants, `cfmakeraw`, stub `tcgetattr`/`tcsetattr`
  (return `ENOTTY` — Switch has no TTY), `ptsname`.
- `ifaddrs.h` — `struct ifaddrs` + `getifaddrs`/`freeifaddrs` decls.
- `sys/uio.h`, `sys/un.h`, `sys/utsname.h` (→ "Horizon"), `sys/statfs.h`
  (struct only), `dlfcn.h` (stub).

## Why `-D__SWITCH__` is required at build time
`__SWITCH__` is **not** a devkitA64 GCC built-in — it's normally defined by the
devkitPro application Makefile. The CMake toolchain does not define it, so the
PKGBUILD passes `-D__SWITCH__` (and `-D_GNU_SOURCE -DSSIZE_MAX=...`) via
`CMAKE_C_FLAGS`; embedders must do the same (the pkg-config Cflags include it).

## Consuming the package
Use the installed pkg-config file, which encodes all of the above:

```sh
source /opt/devkitpro/switchvars.sh
CFLAGS="$(aarch64-none-elf-pkg-config --cflags libuv)"
LIBS="$(aarch64-none-elf-pkg-config --libs libuv) -lnx"
```

`--libs` expands to `… /libuv-horizon-port.o -luv`, so the support object is
linked automatically. See `share/switch-libuv/example.sh`.

## Hardware validation
Validated on a real Nintendo Switch (FW 18.1.0, Atmosphère), `OVERALL: PASS`
on two test NROs covering:

- **Event loop core**: `uv_default_loop`, repeating `uv_timer` + `uv_hrtime`,
  `uv_async` cross-wakeup, and a non-blocking `uv_tcp_connect` whose result is
  delivered through `uv__io_poll` (connection-refused callback, status -111) —
  confirms the loop, timers, async self-pipe, and posix-poll socket backend.
- **Thread pool** (`uv_queue_work`): work runs on a distinct worker thread and
  the after-callback is dispatched back on the loop thread.
- **Filesystem** (`uv_fs_open`/`write`/`read`/`close`/`unlink` on `sdmc:/`,
  all via the thread pool): full write→read roundtrip verified byte-for-byte.
- **DNS** (`uv_getaddrinfo`): resolves through the thread-pool resolver.
- **TCP server** (`uv_listen`/`uv_accept`/`uv_read_start`/`uv_write`): a libuv
  TCP echo server benchmarked from a PC with `tcpkali`. Sustained ~28 MB/s of
  echo throughput per direction (~230 Mbps RX), 102 connections handled with
  zero accept errors and byte-symmetric echo (884 MB in / 831 MB out). This is
  the test that surfaced the `uv__cloexec` bug above.

Platform notes (not libuv issues):
- The Switch `bsd:` service caps **concurrent** connections at ~25 (a firmware
  session-pool limit driven by the socket buffer config); connections beyond
  that don't complete their handshake (no accept error is raised).
- It does not deliver same-process `127.0.0.1` loopback *accepts*, so an
  in-process listen→connect→accept never completes (a loopback echo
  server+client in one process would hang). Distinct peers are unaffected.
- Use a socket config with adequate buffers, e.g. the values in
  `examples/` (1 MB/4 MB TCP buffers, `sb_efficiency=8`,
  `bsd_service_type=BsdServiceType_Auto`); overly small buffers break
  accept/poll readiness on the firmware.
