# Porting V8 to Nintendo Switch (devkitA64 / libnx)

Status: **research / proof-of-concept**. This is NOT a finished port. It is a
plan of record and a place to track blockers, decisions, and progress.

Goal (stated by maintainer): ultimately **full JIT** V8 on Switch, using libnx
`jit_*` APIs for executable memory. Jitless mode is acceptable as a first
milestone / proof of concept.

---

## TL;DR feasibility

| Concern | QuickJS (existing) | V8 (this effort) |
| --- | --- | --- |
| Source | single tarball | depot_tools + `gclient sync` (~10GB) |
| Build system | CMake | GN + Ninja |
| Compiler | devkitA64 GCC ok | **Clang only** (GCC unsupported) |
| Memory model | malloc | `mmap`/`mprotect` (absent on devkitA64) |
| Executable memory | none (interpreter) | RWX or W^X dual-map (Horizon restricts) |
| Host tool needed | none | `mksnapshot` (cross-compile) |
| Platform layer | portable C | `src/base/platform/` POSIX, not Horizon-ready |

V8 is the opposite of "embeddable + portable." Every assumption in this repo's
PKGBUILD conventions (single source tarball, CMake/Meson, devkitA64 GCC) is
violated. A real port is a multi-week+ engineering project, not a config tweak.

---

## Confirmed local environment facts

- devkitA64 GCC: **15.2.0** (V8 needs Clang to compile itself).
- Host: **macOS arm64** (Darwin). `mksnapshot` host binary = macOS arm64.
- Host Clang: Apple Clang 17 (Darwin-centric) AND **Homebrew LLVM 22.1.5** at
  `/opt/homebrew/opt/llvm` (preferred — full upstream LLVM w/ `lld`, `llvm-ar`).
- `gn`, `ninja`: **NOT installed** on host. Must be provided (depot_tools).
- libnx: provides `jit.h`, `virtmem.h`, `thread.h`, `mutex.h`, `condvar.h`.
- devkitA64 newlib: **NO `sys/mman.h` / `mmap`** (checked). `pthread.h` exists.

### M0 hardware results (jit-poc.nro on real Switch)

Confirmed on actual hardware:
- `jitCreate` succeeds (rc=0).
- **`jit.type == JitType_CodeMemory` (type 1)** — i.e. `rw_addr != rx_addr`.
  Example run: `rw=0x4fef079000  rx=0x25b74d0000`. This is the HARDER case;
  `SetProcessMemoryPermission` (rw==rx, V8-friendly) is NOT available to
  homebrew here. We must build the write-redirection layer (#4 below).
- BOTH tests PASS, including the V8-critical test: code performing a
  **PC-relative `LDR` literal load of an absolute self-pointer**, where the
  pointer was computed against `rx_addr` and the bytes written through the
  `rw_addr` alias. Conclusion (validated on hardware):

  > Generated code executes correctly from the rx alias **provided every
  > self-reference (literals, absolute pointers) is computed against
  > `rx_addr`**, while the bytes are written through `rw_addr`.
  > Patch rule: to modify execute-address E, write to `E - rx_addr + rw_addr`.
  > The (rx - rw) delta is constant per Jit buffer.

This proves full JIT is mechanically viable on this firmware; remaining work is
integrating this patch/redirect rule into V8's code-emission and code-patching
paths.

---

## The blockers, in priority order

### 1. Compiler: V8 requires Clang — ✅ SOLVED (recipe validated)
V8 only supports Clang. devkitA64 ships GCC 15.2.0. Resolved by using an
upstream LLVM Clang as the *compiler* and the devkitA64 GCC driver as the
*linker*. Full verified recipe is in `toolchain/clang-flags.sh`; key points:

- **Use upstream LLVM Clang, NOT Apple Clang.** Apple Clang can emit aarch64
  ELF but is Darwin-centric; upstream LLVM (here: Homebrew `llvm` 22.1.5) ships
  `lld`/`llvm-ar`/etc. and matches V8's expectations. Verified it compiles
  `std::vector` etc. against devkitA64 libstdc++.
- **TLS / `-mtp=soft` (the subtle blocker):** devkitPro GCC uses `-mtp=soft`
  because Horizon TLS reads `TPIDRRO_EL0` (read-only). Stock Clang has **no**
  `-mtp=soft` and emits a raw `mrs x, TPIDR_EL0` for `thread_local`, which is
  WRONG for Switch. **Fix: `-femulated-tls`.** Clang then emits calls to
  `__emutls_get_address`, which devkitA64 `libgcc.a` provides (pthread-key
  based -> goes through libnx -> `TPIDRRO_EL0`). Verified: object references
  `__emutls_get_address`, no raw TPIDR; links and resolves from libgcc.
- **Include ordering (the other subtle blocker):** must use `-nostdinc
  -nostdinc++` and supply, IN ORDER: (1) Clang's own resource-dir include
  (for `arm_acle.h` and intrinsics — GCC's version uses `__builtin_aarch64_*`
  that Clang lacks and will error via `switch.h` -> `crypto/crc.h`),
  (2) devkitA64 libstdc++ headers, (3) newlib headers, (4) libnx headers.
  Avoid leaking Homebrew libc++ (causes `"No thread API"` / availability
  errors).
- **Linking:** compile with Clang, **link with `aarch64-none-elf-g++`**
  (`-specs=.../switch.specs ... -mtp=soft -lnx`). `lld` is not required/present.
- **Validated end-to-end:** a `thread_local`-using C++ TU compiled by Clang +
  linked by the GCC driver produces a valid AArch64 Switch PIE `.elf` with
  `__emutls_get_address` resolved. See `toolchain/clang-flags.sh --print`.

Remaining sub-task: teach GN's toolchain definition to use exactly these flags
(blocker #6 / GN `horizon` toolchain).

### 2. Build system: GN + Ninja + depot_tools
- No source tarball. Need `fetch v8` / `gclient sync` to assemble the tree and
  its DEPS. This does not fit `makepkg`'s `source=()`+`sha256sums=()` model
  cleanly. Likely need a vendored/mirrored snapshot of a pinned V8 commit + its
  required DEPS, or a `source` entry that is a pre-assembled tarball we host.
- Pin a specific V8 version for reproducibility. **Pinned: `15.0.243`** — the
  newest upstream tag (verified via `git ls-remote --tags` against
  chromium.googlesource.com/v8/v8). Note: V8 trunk tags (15.0.x) run ahead of
  the current Chromium *stable* milestone (M148 = V8 branch 14.8). Newest is
  preferred here because the `WritableJitAllocation` write path we hook for the
  rw/rx redirection is most consolidated on recent trunk.

### 3. Memory management: no mmap on devkitA64
V8's `platform-posix.cc` uses `mmap`/`mprotect`/`munmap`/`madvise` for:
- the GC'd heap (reserve large address space, commit on demand),
- code space allocation.
None of this exists in devkitA64 newlib. Must implement a Horizon backend
(`platform-horizon.cc` or shims) on top of libnx `virtmem.h`.

**libnx memory primitives (the toolbox for this backend):**
- Reserve/commit: `virtmem.h` (`virtmemFindAslr`/`virtmemAddReservation`) plus
  the SVCs `svcMapMemory`/`svcUnmapMemory` (alias) and `svcSetMemoryPermission`
  for permission changes.
- **Process/region info via `svcGetInfo(u64* out, id0, CUR_PROCESS_HANDLE, 0)`**
  (ref: nx.js uses this pattern; libnx `svc.h`). Useful `InfoType_*`:
  - `TotalMemorySize` (6), `UsedMemorySize` (7) — total/used process memory.
  - `AslrRegionAddress`/`AslrRegionSize` (12/13) — whole address space.
  - `HeapRegionAddress`/`HeapRegionSize` (4/5),
    `AliasRegionAddress`/`AliasRegionSize` (2/3),
    `StackRegionAddress`/`StackRegionSize` (14/15).
  These define where we may map and how big the heap/alias regions are — needed
  to drive a V8 `PageAllocator` / address-space reservation on Horizon.
- ALREADY APPLIED (patch 0003): `src/base/sys-info.cc` now reports real numbers
  via `svcGetInfo` — `AmountOfPhysicalMemory` -> `TotalMemorySize`,
  `AmountOfVirtualMemory` -> `AslrRegionSize` (instead of stubbing 0).

### 4. Executable memory / JIT (the hard one for full JIT)
libnx `jit.h` model (see `/opt/devkitpro/libnx/include/switch/kernel/jit.h`):
- `jitCreate(Jit*, size)` reserves a region with **two aliases** of the same
  physical memory: `rw_addr` (writable) and `rx_addr` (executable).
- You write code through `jitGetRwAddr()`, then `jitTransitionToExecutable()`,
  then run via `jitGetRxAddr()`.
- This is a **W^X dual-mapping**: write address != execute address.

V8's assembler/code-gen assumes generated code is **written to and executed
from the same virtual address** (PC-relative branches, embedded absolute
pointers, ICs, relocation all computed against the final exec address). The
dual-address model breaks this assumption.

**Hardware reality (from M0): we get `JitType_CodeMemory`, so `rw != rx`.**
The single-region flip (`SetProcessMemoryPermission`, option b) is NOT
available, so we must implement the redirection (option a). M0 proved this
works as long as self-references are rx-based.

Validated integration strategy (option a):
1. **All V8 "code addresses" are rx (execute) addresses.** Relocation, PC-rel
   branches, embedded pointers, IC patching — everything V8 computes — uses the
   rx address space. This is what V8 already does internally (it has one notion
   of "the code's address"); we just make that address be `rx`.
2. **Allocate code space from one (or a pool of) Jit buffers.** A Horizon code
   allocator hands out ranges within `[rx_addr, rx_addr+size)`. Record the
   constant `delta = rw_addr - rx_addr` per buffer.
3. **Redirect writes.** Every place V8 *writes* into code memory must translate
   the target rx address `E` to the rw alias `E + delta` before storing. The
   chokepoints in modern V8:
   - `WritableJitAllocation` / `WritableJitPage` (the jit-allocation API in
     `src/common/code-memory-access*.{h,cc}`) — this is the modern, centralized
     write path and is the BEST hook point. Implement its writes to go through
     the rw alias.
   - `RwxMemoryWriteScope` / `CodePageMemoryModificationScope` — on Horizon
     these become no-ops for permission flipping (libnx already keeps rw mapped
     writable); the actual W^X is handled by the alias separation.
   - Assembler buffer relocation / `CopyBytes` into final code: route through
     the rw alias.
4. **Cache coherency.** After writing via rw and before executing via rx, ensure
   I/D cache sync. `jitTransitionToExecutable` handles the permission/coherency
   transition; for incremental patching we may also need explicit
   `armDCacheFlush`/`armICacheInvalidate` over the rx range. (libnx
   `arm/cache.h`.) M0 did full transitions; incremental patching needs
   validation.

Risk: V8 has *many* code-write sites. The modern `WritableJitAllocation` work
(post ~2023) consolidated most of them, which is why pinning a recent V8 is
important — older V8 scatters writes and would be far more invasive.

### 5. Snapshot / mksnapshot cross-compile
V8 builds `mksnapshot` which runs on the **host** to produce a startup snapshot
embedded in the target binary. Requires a GN cross-compile config with a host
toolchain (macOS arm64 Clang) AND a target toolchain (aarch64-none-elf). For a
first PoC we can set `v8_use_snapshot=false` / build without embedded snapshot
to avoid the cross mksnapshot problem, at a startup-cost penalty.

### 6. Platform layer details
`src/base/platform/` needs Horizon impls for: threads (libnx `thread.h`),
mutex/cond (`mutex.h`/`condvar.h`), TLS, `TimezoneCache`, monotonic/clock time,
`Sleep`, stack guard / stack size, `NumberOfProcessors`, etc. newlib gives some
of this; the rest maps to libnx. `pthread.h` exists so the posix thread path
may partially work, but should be validated.

---

## Proposed milestone plan

**M0 — JIT memory PoC (standalone, no V8). ✅ DONE — passed on hardware.**
libnx homebrew (`jit-poc/`): `jitCreate`, write AArch64 code via rw alias,
`jitTransitionToExecutable`, call via rx alias. Tests both a trivial
`movz/ret` and a PC-relative `LDR` literal with an rx-based absolute
self-pointer. Result on real Switch: `JitType_CodeMemory` (rw != rx), BOTH
tests PASS. De-risks #4: dual-map execution works with rx-based references +
rw-aliased writes.

**M1 — Jitless V8 builds + links (proof of concept).**
- Obtain Clang-for-aarch64-none-elf toolchain.
- GN args: `v8_enable_lite_mode=true` (implies jitless), `v8_use_snapshot=false`
  (or external snapshot), no pointer compression initially, single-threaded GC
  where possible.
- Implement minimal Horizon memory backend (reserve/commit via virtmem, no exec).
- Goal: link a static `libv8_monolith.a` against devkitA64 newlib and run a
  trivial "evaluate 1+1" in an .nro.

**M2 — Full platform backend hardening.**
Threads, time, TLS solid. Run V8's basic API smoke tests.

**M3 — Full JIT via libnx jit_*.**
Implement the rw/rx alias redirection in V8's code-space write paths
(`CodePageMemoryModificationScope` and friends) using the M0 findings.
Switch GN args to JIT-enabled. This is the real prize and the hardest step.

---

## Why this won't be a normal PKGBUILD (yet)

Until M1 reliably produces `libv8_monolith.a`, a `makepkg`-style PKGBUILD is
premature. The PKGBUILD here is a scaffold to capture the intended invocation;
it will not succeed until the toolchain (Clang) and source-assembly story are
solved. See `PKGBUILD` in this directory.
