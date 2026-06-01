# switch-libuv build notes

libuv **v1.52.1** ported to Nintendo Switch (Horizon / devkitA64 + libnx).

Built with the **stock devkitPro GCC toolchain** (`/opt/devkitpro/cmake/Switch.cmake`)
— no external clang required. The library uses libuv's generic POSIX event
backend (`src/unix/posix-poll.c`) over libnx's BSD sockets + `poll()`.

## What the port consists of

### 1. Source patch — `0001-horizon-switch-port.patch`
Applied to the upstream `dist.libuv.org` tarball. Four changes:

- **`CMakeLists.txt`**: a `Horizon`/`NintendoSwitch` `CMAKE_SYSTEM_NAME` branch
  selecting the source set:
  `horizon.c posix-hrtime.c posix-poll.c bsd-ifaddrs.c no-proctitle.c no-fsevents.c`.
- **`include/uv/unix.h`**:
  - route `__SWITCH__` to `#include "uv/posix.h"` (gives the loop the
    `poll_fds*` fields the posix-poll backend needs);
  - define `struct ipv6_mreq` right after `<netinet/in.h>` (libnx defines
    `IPV6_JOIN_GROUP`/`struct in6_addr` but omits `ipv6_mreq`).
- **`src/unix/core.c`**: guard the extended `getrusage` field copies
  (`ru_maxrss`, `ru_minflt`, …) with `!defined(__SWITCH__)` — newlib's
  `struct rusage` only has `ru_utime`/`ru_stime`. They stay zero in
  `uv_rusage_t`, which is honest (Switch doesn't track them).
- **`src/unix/horizon.c`** (new): the per-platform "util" file. Memory queries
  via `svcGetInfo` (`Total`/`Used` memory), `uv_uptime` via `armGetSystemTick`
  (19.2 MHz), `uv_cpu_info` reporting the 4 Cortex-A57 cores, `uv_loadavg`/
  `uv_exepath` no-ops. `uv__io_poll` and the loop init/delete come from
  `posix-poll.c`.

### 2. libc support object — `horizon-port.c` → `libuv-horizon-port.o`
Symbols newlib **declares but does not implement** on Horizon. Two kinds:

- **Implemented for real** on top of `lseek`/`read`/`write`:
  `pread`, `pwrite`, `readv`, `writev`.
- **Stubbed** (Switch has no fork/exec, POSIX signals, ownership, TTYs,
  thread naming, process priority, interface enumeration) — they make the
  relevant `uv_*` APIs fail cleanly or return defaults rather than fail to
  link: `getrusage`, `sigaction`/`sigprocmask`/`pthread_sigmask`, `waitpid`/
  `execvp`/`pipe`, `geteuid`/`getppid`/`setuid`/`setgid`/`setsid`/`setgroups`,
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
A test NRO (uv_default_loop + repeating timer + `uv_hrtime` + async handle +
loopback TCP echo server/client in one loop) links cleanly with zero undefined
references. On-hardware run pending (console was offline at package time).
