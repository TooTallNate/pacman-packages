# V8 -> Switch build log

Reproducible record of porting V8 `15.0.243` to the Nintendo Switch with the
devkitA64 cross toolchain.

## 🎉 M1 COMPLETE: V8 reliably runs JavaScript on the Switch

`hello-v8.nro` on real hardware (FW 18.1.0, Atmosphère) — **11/11 runs
succeeded** (verified after the stack fix below; before it, success was an
intermittent coin-flip):
```
... -> Isolate::New done -> [stack limit set] -> Context::New done ->
compiling -> running
V8 evaluated '1 + 1' = 2
SUCCESS: V8 ran on Switch!
```
A full jitless V8 engine, cross-compiled from source, parsing and executing JS
on the console — and **exiting cleanly back to the homebrew menu**.

### Clean exit: release manual svcMapMemory mappings on shutdown

Pressing `+` to exit initially crashed hbmenu. Disassembling the actual fault
in `hbmenu.nro` (pulled via FTP) showed PC in libnx `armDCacheFlush`
(`dc civac`) over a null framebuffer pointer during hbmenu's console flush —
i.e. hbmenu's graphics couldn't map its framebuffer. Cause: libnx's NRO exit
does NOT unmap manual `svcMapMemory` regions (only thread stacks; confirmed in
libnx thread.c/exit). Our memory arena's aliases + the `virtmemAddReservation`
leaked into the next process and occupied the address space hbmenu needed.
Fix: `horizon_mman_teardown()` (mman-horizon.cc) unmaps every arena mapping and
removes the reservation; the embedder calls it after `consoleExit`, just before
returning to hbloader. Result: clean return to the homebrew menu.

### THE FINAL (subtle) BUG: stack start used the wrong libnx alias

First successes were INTERMITTENT — `1 + 1` sometimes threw
`RangeError: Maximum call stack size exceeded` at compile time. Root cause: a
libnx thread has TWO addresses for its stack — `stack_mem` (heap backing) and
`stack_mirror` (the svcMapMemory alias the thread ACTUALLY RUNS ON, with SP in
that range). `ObtainCurrentThreadStackStart()` returned `stack_mem + stack_sz`
(wrong alias), so V8's `limit = stack_start - stack_size` was relative to a
different address region than the real SP. ASLR randomizes the mirror each
launch, so whether the bogus limit sat above/below the real SP was a coin flip
-> intermittent spurious overflow. Fix (patch 0003, platform-posix.cc): return
`stack_mirror + stack_sz`. Belt-and-suspenders: the embedder also calls
`isolate->SetStackLimit()` anchored to the real current SP.

### Runtime fixes that got it from "links" to "runs" (in order discovered)

All crashes were decoded from Atmosphère crash reports
(`sdmc:/atmosphere/crash_reports/`), verified ours by matching the module
build-id, then symbolized against `hello-v8.elf` (nearest-symbol via `nm` +
disassembly around the fault PC, since `symbol_level=0`).

1. **Worker-thread teardown crash** (`absl::CondVar::WaitCommon` via
   `DefaultWorkerThreadsTaskRunner`). Fix: `NewSingleThreadedDefaultPlatform()`
   + `--single-threaded --single-threaded-gc --predictable` (no worker pool).
2. **cppgc 4 GB cage reservation** OOM in `V8::Initialize`. Fix GN arg
   `cppgc_enable_caged_heap=false`.
3. **Memory backend: malloc-backed mmap can't partial-unmap.** V8's
   `OS::Allocate` over-allocates then frees the unaligned prefix/suffix; a
   malloc backing freed the whole block. Rewrote `horizon/mman-horizon.cc` as a
   faithful backend: reserve a stack-region arena (`virtmemFindStack` +
   `virtmemAddReservation`), commit per-allocation via `svcMapMemory` of heap
   backing, decommit via `svcUnmapMemory`. (Per-PAGE mapping was tried first and
   hit kernel `ResourceExhausted` 0xce01 after ~7000 mappings -> switched to
   per-allocation. Adaptive arena sizing 1G/512M/256M/... to fit the stack
   region.)
4. **Partial-unmap wiped live data** (`ReadOnlyPage::ShrinkToHighWaterMark` ->
   `PartialFreeMemory` -> null deref). Fix: `Decommit` saves survivor
   prefix/suffix bytes and restores them after re-mapping.
5. **Run V8 on a dedicated libnx thread with an 8 MB stack** (hbloader main
   thread is ~1 MB and not a libnx Thread, so V8's stack guard couldn't be set).
   Plus thread-safe `locked_print` (consoleUpdate isn't thread-safe).
6. **`SegmentedTable::InitializeTable` OOM** — V8's JSDispatchTable tried to
   reserve **256 MB**. Fix GN arg `v8_lower_limits_mode=true` (drops it to
   16 MB; the intended knob for memory-constrained devices). THIS unblocked
   `Isolate::New`.
7. libc gaps + atomic + abseil-sync (see "LINK-time gaps" below).

Debugging infra (in `hello-v8/main.cc` and a temporary `logging.cc` patch):
`[ck]` checkpoints; `locked_print` mirrors to `sdmc:/hello-v8-out.log`;
`FatalOOM`/`FatalNoSecurityImpact`/`V8_Fatal` patched to write the abort message
to `sdmc:/hello-v8-fatal.log` via low-level open/write (stdio is wedged in the
fatal context); `mman` arena/map activity to `sdmc:/hello-v8-mman.log`.

Working GN args are saved in `gn-args.txt`. Key correctness args beyond the
toolchain: `v8_jitless=true v8_enable_lite_mode=true v8_enable_sandbox=false
v8_enable_pointer_compression=false cppgc_enable_caged_heap=false
v8_lower_limits_mode=true v8_monolithic=true v8_use_external_startup_data=false`.

---

## Earlier status (build/link milestones)

- ✅ depot_tools + `fetch v8` + checkout tag `15.0.243` + `gclient sync`.
- ✅ Host (macOS, Command Line Tools only) unblocked for gn.
- ✅ Custom GN `horizon` toolchain (bundled Clang compile + devkitA64 g++ link).
- ✅ `gn gen` succeeds for the Switch target (881 targets).
- ✅ Toolchain emits our exact validated cross command (verified in
  `toolchain.ninja`): bundled clang++, `--target=aarch64-none-elf`, Switch
  arch flags, `-femulated-tls`, `-D__SWITCH__`, `-nostdinc` sysroot includes.
- ✅ **A V8 embedder LINKS and produces a runnable `.nro`.**
  `switch/v8/hello-v8/` evaluates `1 + 1` in a real `v8::Isolate`. Links
  `libv8_monolith.a` + a combined abseil archive + zlib/compression_utils +
  `-lnx` into an 11 MB `hello-v8.nro` (AArch64 PIE) with ZERO undefined symbols.
  See `hello-v8/build.sh`. (Not yet run on hardware.)
- ✅ **`ninja v8_monolith` FULLY BUILDS → `libv8_monolith.a` (37 MB, 1047
  AArch64 ELF64 objects, real `v8::` API symbols).** A complete jitless V8
  engine static library for Nintendo Switch. `v8_libbase` and `v8_libplatform`
  also build standalone.
- ✅ **`ninja v8_libbase` builds** → `libv8_libbase.a` (42 AArch64 objects).
  The base/platform library cross-compiles against devkitA64/libnx/newlib.
  Fixed along the way:
  - `V8_OS_HORIZON`/`V8_OS_POSIX` added (`include/v8config.h`) — unblocks
    `semaphore.h` `NativeHandle` (= `sem_t`) and all `V8_OS_*` dispatch.
  - `-D_DEFAULT_SOURCE` in the toolchain — newlib hides POSIX/SVID symbols
    (`_timezone`, `tzname`, signal helpers) under `-std=c++20` strict-ANSI.
  - Missing includes: `<climits>` (CHAR_BIT) in `memcopy.h`, `<cstdarg>`
    (va_list) in `strings.h`.
  - newlib `struct tm` has no `tm_gmtoff`/`tm_zone`: patched abseil
    `time_zone_libc.cc` (use `_timezone`/`tzname` via the NaCl-style branch) and
    V8 `platform-posix-time.cc` (Horizon branch using `_timezone`/`tzname`).
  - `sys-info.cc`: real libnx `svcGetInfo` for physical/virtual memory.
  - **The platform layer (the substantive part):**
    - `src/base/platform/horizon/sys/mman.h` + `mman-horizon.cc` — a minimal
      `<sys/mman.h>` shim (newlib has none). Anonymous mappings backed by
      `memalign` (no demand paging on Horizon); `mprotect`/`madvise` are
      no-ops; `MAP_FIXED` unsupported. Enough for jitless bring-up. (libnx
      jit.c/virtmem.c source confirms this is how libnx itself bootstraps code
      buffers; a faithful svcMapMemory-over-virtmem version is a follow-up.)
    - `src/base/platform/platform-horizon.cc` — the per-OS file V8 requires
      (CreateTimezoneCache, GetSharedLibraryAddresses, SignalCodeMovingGC,
      AdjustSchedulingParams, GetFirstFreeMemoryRangeWithin).
    - `src/base/debug/stack_trace_horizon.cc` — no-backtrace stack trace
      (modeled on fuchsia; newlib has no execinfo).
    - `platform-posix.cc` Horizon branches: skip `<sys/syscall.h>`,
      `GetPeakMemoryUsageKb` returns -1 (no `ru_maxrss`), `PTHREAD_STACK_MIN`
      fallback, `ObtainCurrentThreadStackStart` via libnx `threadGetSelf()`.
    - `BUILD.gn`: `is_horizon` branch selecting the above + the shim include dir.
  - abseil (Linux/ELF introspection NOT needed on Switch): exclude `__SWITCH__`
    from `ABSL_HAVE_ELF_MEM_IMAGE` (drops elf_mem_image + vdso_support), add a
    `__SWITCH__` `GetTID` (pthread_t is a pointer in newlib), and exclude
    `__SWITCH__` from `ABSL_HAVE_SIGACTION` (no SA_SIGINFO/sa_sigaction).
  - `src/libsampler/sampler.{h,cc}`: exclude `__SWITCH__` from `USE_SIGNALS`
    (no `<ucontext.h>` / signal sampling on Horizon) and add an empty
    `PlatformData` for the Horizon branch. CPU-profiler signal sampling is
    therefore disabled on Switch (fine for bring-up).
  - Toolchain `copy` tool: `cp -af` not `cp -afd` (macOS host `cp` has no `-d`;
    surfaced when copying `icudtl.dat`).

- LINK-time gaps fixed (resolving the embedder .nro):
  - newlib has no `sysconf`/`posix_memalign`/`pthread_sigmask`: implemented in
    `horizon/libc-horizon.cc` (sysconf returns 4 KiB page / 3 cpus /
    TotalMemorySize pages; posix_memalign wraps aligned_alloc; pthread_sigmask
    is a no-op).
  - devkitA64 has no libatomic and libgcc lacks the GENERIC (variable-sized)
    `__atomic_compare_exchange` that V8 emits: implemented in
    `horizon/atomic-horizon.cc` (spinlock-pool CAS; defined via an `__asm__`
    label since Clang reserves the builtin name).
  - abseil's LowLevelAlloc / thread-identity / per-thread-sem were compiled OUT
    because `ABSL_HAVE_MMAP` was undefined (no mmap) -> cascaded to undefined
    `CreateThreadIdentity` etc. Fix: define `ABSL_HAVE_MMAP` for `__SWITCH__`
    (our mman shim suffices) and make the shim's `<sys/mman.h>` visible to ALL
    targets via `build/config/compiler:default_include_dirs` (is_horizon).
  - abseil objects are NOT bundled in `libv8_monolith.a`; the embedder links a
    combined abseil archive built from `out/.../abseil-cpp/**/*.o`.

## Runtime debugging on hardware (in progress)

Ran `hello-v8.nro` on a real Switch (FW 18.1.0, Atmosphère). Crashes decoded by
subtracting the dump's "Backtrace Start Address" from PC and running addr2line
on `hello-v8.elf`. Findings so far:

1. **Crash #1 — worker-thread teardown.** PC symbolized to
   `absl::CondVar::WaitCommon`, backtrace `DefaultWorkerThreadsTaskRunner::
   WorkerThread::~WorkerThread`. Fix: use `NewSingleThreadedDefaultPlatform()`
   + `--single-threaded --single-threaded-gc --predictable` (no worker pool).
2. **Crash #2 — 4 GB cppgc cage reservation.** Got to `[ck] platform
   initialized`, then aborted in `v8::V8::Initialize()`. PC symbolized to
   `OS::Abort` <- `FatalNoSecurityImpact` (a deliberate V8 abort, not memory
   corruption). Root cause: cppgc reserves a **4 GB caged heap**
   (`kCagedHeapDefaultReservationSize`, doubled to 8 GB with
   `CPPGC_POINTER_COMPRESSION`). Our malloc-backed mmap shim can't `memalign`
   4-8 GB, so the reservation fails -> fatal. Fix: GN arg
   `cppgc_enable_caged_heap=false` (also disables cppgc young-gen + pointer
   compression, per the BUILD.gn asserts). Removes the giant reservation.

Diagnostics added to `hello-v8/main.cc`: `[ck]` checkpoints (printf+flush+sleep
at each init stage), `freopen` of stderr to `sdmc:/hello-v8-stderr.log` (V8's
OS::PrintError uses stderr, invisible on console and unflushed before abort),
and `SetFatalErrorHandler`/`SetDcheckErrorHandler` to print V8 fatals to stdout.

DONE for M1: a jitless V8 LINKS into a Switch `.nro` and begins initializing on
real hardware. Caveats: the mman shim is malloc-backed (no demand paging, no
MAP_FIXED) and `mprotect` is a no-op. This is why cppgc's cage had to be
disabled; the GC/PageAllocator will ultimately want the faithful
svcMapMemory-over-virtmem backing.

3. **Crash #3 — malloc-backed mmap can't do partial unmap.** Got to
   `[ck] allocator created`, then a HARD fault (no stderr log written) in
   `v8::Isolate::New()`. Root cause: V8's `OS::Allocate` over-allocates then
   `Free()`s the unaligned prefix/suffix to satisfy large (256 KB-2 MB)
   alignment. Our malloc-backed `munmap` did `free()` on the whole block, so
   freeing a sub-range freed the entire allocation -> use-after-free on the
   aligned region. Also `mmap(addr, PROT_NONE, MAP_FIXED)` decommit was
   unsupported. FUNDAMENTAL: a malloc backing cannot do partial unmap.

   FIX: rewrote `horizon/mman-horizon.cc` as a FAITHFUL backend (per the
   maintainer's call). Reserves a 512 MiB address-space arena via
   `virtmemFindAslr` + `virtmemAddReservation` (no commit). `mmap` commits
   page-aligned sub-ranges by `memalign`-ing heap backing and aliasing it with
   `svcMapMemory(dst, src, size)` (the same primitive libnx uses for thread
   stacks). `munmap`/`madvise(DONTNEED)` decommit via `svcUnmapMemory`, with
   region splitting so partial unmaps re-commit the surviving prefix/suffix.
   `mprotect` maps RW/RO/NONE onto `svcSetMemoryPermission` (exec stays the
   libnx jit_* path). Region bookkeeping is a simple linked list under a libnx
   mutex; allocation is a bump pointer within the arena.

Next frontiers (in order):
1. Continue running `hello-v8.nro` with the faithful backend; decode each abort
   via checkpoints + `sdmc:/hello-v8-stderr.log`.
   - Watch for arena exhaustion: the bump allocator does not recycle freed
     holes, and V8's alignment-trim wastes up to `alignment` per allocation. If
     `1+1` exhausts 512 MiB, add a free-list / make Commit alignment-aware.
2. Harden the memory backend (faithful mmap via svcMapMemory + virtmem;
   PageAllocator correctness).
3. Full JIT: libnx jit_* code-write redirection (rw alias writes, rx execution,
   armDCacheFlush/armICacheInvalidate per patch — confirmed via libnx jit.c).

## Environment

- Host: macOS arm64, Command Line Tools only (no full Xcode).
- devkitPro at `/opt/devkitpro` (devkitA64 GCC 15.2.0, libnx).
- V8 brings its OWN Clang 23 in `third_party/llvm-build/Release+Asserts`.
  We use that as the compiler (better than Homebrew LLVM for V8).
- Build tree lives OUTSIDE this repo (huge): a temp dir. Do NOT commit it.

## Reproduce

```sh
WORK=/var/folders/.../opencode/v8build      # any scratch dir w/ ~15GB free
mkdir -p "$WORK" && cd "$WORK"
git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$WORK/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=0

fetch --no-history v8
cd v8
git fetch --depth 1 origin refs/tags/15.0.243 && git checkout FETCH_HEAD
gclient sync --no-history -D
```

### Host unblock (CLT-only mac)

V8's mac build calls `xcodebuild` and expects an Xcode.app SDK layout. With
only Command Line Tools, create shims:

- a fake `xcodebuild` that prints `Xcode 16.0` / a build version, on PATH;
- a fake `xcode-select` that prints a fake Developer dir on PATH;
- a fake Developer dir whose
  `Platforms/MacOSX.platform/Developer/SDKs/MacOSX<ver>.sdk` and
  `Toolchains/XcodeDefault.xctoolchain/usr/bin/{clang,...}` symlink to
  `/Library/Developer/CommandLineTools/SDKs/MacOSX<ver>.sdk` and
  `/Library/Developer/CommandLineTools/usr/bin/*`;
- a `Contents/version.plist` with `CFBundleShortVersionString`/`ProductBuildVersion`.

Then pass `mac_sdk_min="<ver>"` (e.g. `26.2`) in gn args. (Only needed for the
HOST toolchain that builds torque/mksnapshot; a Linux CI host with Xcode-like
tooling would not need this.)

### Apply the V8-tree patches

From this directory:

```sh
P=.../patches
( cd "$WORK/v8/build"            && git apply "$P/0001-build-config-horizon.patch" )
( cd "$WORK/v8/third_party/zlib" && git apply "$P/0002-zlib-disable-arm-neon-horizon.patch" )
( cd "$WORK/v8" && git apply \
    "$P/0003-v8-base-newlib-horizon.patch" \
    "$P/0005-v8-buildgn-horizon-platform.patch" \
    "$P/0006-v8-buildgn-horizon-target-os.patch" )
( cd "$WORK/v8/third_party/abseil-cpp" && git apply "$P/0004-abseil-horizon.patch" )
cp .../toolchain/horizon-BUILD.gn "$WORK/v8/build/toolchain/horizon/BUILD.gn"   # mkdir -p first
```

NOTE: `build/`, `third_party/zlib/`, and `third_party/abseil-cpp/` are SEPARATE
git repos (DEPS), hence patches applied in their respective dirs. 0003/0005/0006
apply in the main `v8` repo.

`0006-v8-buildgn-horizon-target-os.patch` is REQUIRED for JIT correctness: it sets
`V8_HAVE_TARGET_OS` + `V8_TARGET_OS_LINUX` for `target_os=="horizon"`. Without it,
v8config.h falls back to the *host* OS (macOS), so `mksnapshot` bakes ARMv8.3
JSCVT/DOTPROD/LSE instructions (e.g. `fjcvtzs`) into the snapshot builtins —
which fault with "Undefined Instruction" on the Switch's ARMv8.0 Cortex-A57.

### gn gen (Switch target)

```sh
LLVM="$WORK/v8/third_party/llvm-build/Release+Asserts"
RES="$LLVM/lib/clang/23/include"
gn gen out/switch --args='
  target_os="horizon" target_cpu="arm64"
  custom_toolchain="//build/toolchain/horizon:aarch64_switch"
  host_toolchain="//build/toolchain/mac:clang_arm64"
  is_clang=true is_debug=false symbol_level=0
  enable_rust=false v8_enable_temporal_support=false
  v8_enable_lite_mode=true v8_jitless=true
  v8_enable_i18n_support=false v8_monolithic=true v8_static_library=true
  use_custom_libcxx=false treat_warnings_as_errors=false
  v8_enable_sandbox=false v8_enable_pointer_compression=false
  v8_use_external_startup_data=false mac_sdk_min="26.2"
  horizon_llvm_dir="'"$LLVM"'" horizon_clang_resource="'"$RES"'"'
ninja -C out/switch v8_libbase   # or v8_monolith
```

## Why each gn arg / patch

| Item | Reason |
| --- | --- |
| `target_os="horizon"` + patch 0001 (BUILDCONFIG) | Register Switch OS; add `is_horizon`; map to our toolchain (else `assert "Unsupported target_os"`). |
| `custom_toolchain` | Our cross toolchain (bundled clang + devkitA64 g++ linker). |
| `enable_rust=false` | V8 pulls Rust (`temporal_capi`); no Rust target for Horizon. |
| `v8_enable_temporal_support=false` | Removes the Rust `temporal_capi` dep entirely. |
| `v8_enable_sandbox=false` | Sandbox requires hardened libc++; we use devkitA64 libstdc++. |
| `v8_use_external_startup_data=false` | Required with `v8_monolithic`. |
| `symbol_level=0` | Avoids `-gsplit-dwarf` `.dwo` outputs our toolchain doesn't declare. |
| patch 0001 (clang_lib) | Skip `clang_rt.builtins`; libgcc provides builtins via the GCC link driver. |
| patch 0001 (compiler_cpu_abi) | Stop V8 injecting `--target=aarch64-linux-gnu` (our toolchain sets `aarch64-none-elf`; last `--target` would otherwise win). |
| patch 0002 (zlib) | zlib ARM CRC/NEON needs getauxval CPU detection absent on Horizon. |
| `use_custom_libcxx=false` | Use devkitA64 libstdc++, not LLVM libc++. |

### zlib: decision and alternatives

Decision: **keep patch 0002 (patch V8's bundled `third_party/zlib`)** for now.
It already builds, keeps the V8 build self-contained, and adds no new plumbing
while we push toward the first successful link.

Context for a future revisit:
- This repo already ships a proper `switch-zlib` port (zlib 1.3.1, installed at
  `/opt/devkitpro/portlibs/switch`, `depends=('switch-zlib')`).
- V8 doesn't only use plain zlib; it also builds
  `third_party/zlib/google:compression_utils_portable`
  (`zlib_internal::CompressHelper`/`UncompressHelper`), which is
  V8/Chromium-specific glue NOT present in upstream zlib. So the system lib
  cannot be a full drop-in for V8's `third_party/zlib` target.
- zlib is functionally needed only for **snapshot compression**
  (`assert(!v8_enable_snapshot_compression || v8_use_zlib)`). Our jitless,
  no-external-startup-data config does not require it, so two cleaner paths
  exist if we want to drop the patch later:
    1. `v8_use_zlib=false` (remove the dependency entirely), or
    2. point core zlib at system `-lz` (`switch-zlib`) while still compiling
       only `compression_utils_portable` from the V8 tree.
  Either can replace patch 0002 once we decide snapshots/compression are out of
  scope or worth the extra wiring.

## Current stopping point (next work)

`ninja v8_libbase` fails compiling
`third_party/abseil-cpp/.../time_zone_libc.cc`:

```
error: no member named 'tm_gmtoff' in 'tm'
```

newlib's `struct tm` lacks `tm_gmtoff`/`tm_zone` (glibc/BSD extensions). Options:
1. Provide a shim/define so abseil's `tm_gmtoff()` SFINAE finds a fallback
   (abseil already probes `tm_gmtoff` and `__tm_gmtoff`; neither exists in
   newlib). Could patch abseil to a `0`-offset fallback for `__SWITCH__`.
2. Configure V8 to not pull abseil time (may not be separable).
3. Add `tm_gmtoff` to a newlib-compatible `struct tm` via a wrapper (risky).

This is the first of the expected newlib/libnx source-porting issues (timezone,
then almost certainly: `mmap`/`mprotect` in platform-posix, missing syscalls,
and the JIT code-write redirection for full JIT — see PORTING-NOTES.md).

## Artifacts in this directory

- `toolchain/clang-flags.sh` — standalone validated flag recipe.
- `toolchain/horizon-BUILD.gn` — the in-tree GN toolchain (copy into
  `build/toolchain/horizon/BUILD.gn`).
- `patches/0001-build-config-horizon.patch` — build/ repo config changes.
- `patches/0002-zlib-disable-arm-neon-horizon.patch` — zlib arch guard.
- `patches/0003-v8-base-newlib-horizon.patch` — newlib/libnx source ports +
  JIT write-redirect sites + fatal logging (main `v8` repo).
- `patches/0004-abseil-horizon.patch` — abseil `tm_gmtoff` fallback + newlib glue.
- `patches/0005-v8-buildgn-horizon-platform.patch` — `v8_libbase` Horizon
  platform sources (mman/atomic/libc/stack-trace).
- `patches/0006-v8-buildgn-horizon-target-os.patch` — set `V8_HAVE_TARGET_OS` +
  `V8_TARGET_OS_LINUX` so mksnapshot does not emit ARMv8.3 instructions (the
  `fjcvtzs` fix). REQUIRED for JIT correctness on the A57.
- `hello-v8/source/main-bench.cc` + `hello-v8/build-bench.sh` — V8(JIT) vs
  QuickJS benchmark embedder and its link recipe.
- `PKGBUILD` — packages the full-JIT build as `switch-v8`. Because V8 cannot be
  a single tarball (depot_tools/gclient + bundled Clang + host tools), it
  expects a pre-fetched checkout via `$V8_SRC` (see the PKGBUILD header). It
  applies patches 0001-0006, drops in `horizon-src/` + the Horizon GN
  toolchain, builds `out/switch-jit`, rebuilds the abseil archive, and installs
  4 static libs + headers + an example link recipe into `$PORTLIBS_PREFIX`.
- `example-link-recipe.sh` — installed to
  `$PORTLIBS_PREFIX/share/switch-v8/`; shows the `--start-group` link of
  `-lv8_monolith -labsl -lchrome_zlib -lcompression_utils_portable` + `-lnx`.

## Milestone: full JIT working + benchmarked vs QuickJS (hardware)

Full Sparkplug+TurboFan V8 runs native AArch64 on hardware (FW 18.1.0,
Atmosphère), passes the 7/7 correctness battery, and beats QuickJS
(quickjs-ng 0.12.1, devkitPro portlib) on every workload:

| benchmark      | V8-JIT  | QuickJS  | speedup | result (both) |
|----------------|--------:|---------:|--------:|---------------|
| fib(32)        |  161 ms |  1613 ms |  10.0x  | 2178309       |
| loop-sum-5M    |   19 ms |   899 ms |  46.2x  | 633038624     |
| string-build   |  4.5 ms |    40 ms |   8.9x  | 20000         |
| array-sort-50k |  142 ms |   183 ms |   1.3x  | 4294873283    |
| mandel-ish     |  193 ms |  7028 ms |  36.3x  | 10000000      |

Tight numeric loops (46x) and FP-heavy code (36x) show the JIT payoff; sort
(1.3x) is the floor since both engines drop into native C for the sort core.
All numeric results match between engines, confirming correctness.

### Two bugs fixed to get the benchmark running

1. **`fjcvtzs` / ARMv8.3 in builtins** — patch 0006 (see above). Verified the
   rebuilt embedded snapshot blob contains **0** `fjcvtzs` (mask `0x1e7e0000`),
   down from 202. `mksnapshot` now reports `JSCVT=0 DOTPROD=0 LSE=0`.
2. **Embedder/V8 build-config mismatch** — the embedder must NOT define
   `V8_COMPRESS_POINTERS` (V8 checks the macro by *presence*, so `=0` still reads
   as ENABLED). Since this V8 is built `v8_enable_pointer_compression=false`,
   `build-bench.sh` defines neither `V8_COMPRESS_POINTERS` nor
   `V8_31BIT_SMIS_ON_64BIT_ARCH`. The same applies to any embedder.

### Building the benchmark

```sh
# 1. Build the JIT monolith into out/switch-jit (gn args = gn-args-jit.txt).
# 2. Rebuild the abseil archive (not bundled in the monolith) to /tmp/libabsl_jit.a:
A64=/opt/devkitpro/devkitA64/bin
find out/switch-jit/obj/third_party/abseil-cpp -name '*.o' \
  | xargs $A64/aarch64-none-elf-ar qc /tmp/libabsl_jit.a
$A64/aarch64-none-elf-ranlib /tmp/libabsl_jit.a
# 3. ./hello-v8/build-bench.sh   (links monolith + libabsl_jit.a + -lqjs + -lnx)
```
