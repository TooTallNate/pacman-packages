# Full JIT on Switch — design + plan

Goal: move from the working **jitless** V8 (Ignition interpreter only) to **full
JIT** (Sparkplug/Maglev/TurboFan generate native AArch64 code at runtime),
using libnx `jit_*` for executable memory.

This is the hardest part of the port. This doc is the plan of record.

## The core problem (validated by the M0 PoC)

On this hardware `jitCreate` returns `JitType_CodeMemory`, which gives **two
distinct virtual addresses** aliasing the same physical pages:
- `rw_addr` — writable, NOT executable
- `rx_addr` — executable, NOT writable
- a constant `delta = rw_addr - rx_addr` per Jit buffer.

V8 (like all JITs) assumes generated code is **written to and executed from the
SAME address**: PC-relative branches, embedded pointers, IC patching, and
relocation are all computed against one "code address". The M0 PoC proved the
rule that makes this work on Horizon:

> Compute ALL self-references against `rx_addr` (the execute address). Write the
> bytes through `rw_addr`. To patch execute-address E, store at `E + delta`.
> After writing, `armDCacheFlush(rw, n)` + `armICacheInvalidate(rx, n)`.

So V8 must keep using the rx address everywhere EXCEPT the actual store
instructions, which must be redirected to `rx + delta`.

## How V8 already abstracts code writes (our leverage points)

Modern V8 (post-CFI work, present in 15.0.243) funnels essentially all code
writes through two abstractions in `src/common/code-memory-access*`:

- **`WritableJitAllocation`** — RAII object representing "I may write to this
  code region now". All builtins/relocation/IC writes go through its
  `WriteValue/WriteHeaderSlot/CopyCode/memcpy`-style methods.
- **`RwxMemoryWriteScope`** — toggles the current thread's ability to write code
  (`SetWritable`/`SetExecutable`). Backends today: Apple `pthread_jit_write_*`,
  PKU memory keys, or plain RWX (no-op; pages are always rwx).

V8's existing backends all assume **one address that flips permissions**. Our
Horizon model is different: two permanent addresses. The cleanest integration:

**Option A (chosen): redirect writes by delta at the WritableJitAllocation
layer.** Keep all of V8's addresses = rx. When `WritableJitAllocation` performs
a store to code address `E`, redirect it to `E + delta`. `RwxMemoryWriteScope`
becomes a near-no-op (the rw alias is always writable). After a writable scope
closes (or per allocation), flush D-cache over the rw range and invalidate
I-cache over the rx range.

This keeps the redirect in ONE place and leaves V8's address arithmetic intact.

## Memory backend changes (horizon)

The current `mman-horizon.cc` arena is RW-only (no exec). For JIT we add an
**executable code arena** backed by libnx `jit_*`:

1. Lazily `jitCreate(&g_code_jit, size)` for a code region (e.g. 64-128 MB; V8
   `kMaximalCodeRangeSize` is lower in `v8_lower_limits_mode`). Record
   `rx = jitGetRxAddr`, `rw = jitGetRwAddr`, `delta = rw - rx`.
2. V8's `CodeRange`/`OS::Allocate(kReadWriteExecute | kNoAccessWillJitLater)`
   must hand out addresses from `[rx, rx+size)`. So the PageAllocator used for
   the code range must allocate from the jit rx region (a dedicated allocator,
   separate from the data arena).
3. `jitTransitionToExecutable` is called once after creation; for
   `JitType_CodeMemory` both aliases stay mapped permanently, so per-write we
   only need cache maintenance, not permission transitions (confirmed in libnx
   jit.c: the CodeMemory path's transitions are just armDCacheFlush /
   armICacheInvalidate).

A global `horizon_jit_delta(addr)` helper returns the rw-vs-rx delta for a code
address so the WritableJitAllocation redirect can find it.

## Build config changes

Turn OFF jitless/lite mode and turn ON the JIT tiers:
- remove `v8_jitless=true v8_enable_lite_mode=true`
- `v8_enable_sparkplug=true`, `v8_enable_maglev` / `v8_enable_turbofan` as
  desired (start with Sparkplug+TurboFan; Maglev later).
- Keep `v8_enable_sandbox=false`, pointer compression off, lower limits on.
- This re-enables the code range / `RRequiresCodeRange` path and a LOT more
  generated code, builtins-as-code, relocation, etc.

## Milestones

- **J0** ✅ DONE: enabled JIT in GN (`out/switch-jit`: drop jitless/lite, add
  `v8_enable_sparkplug=true v8_enable_turbofan=true v8_enable_maglev=false`).
  `libv8_monolith.a` BUILT with 0 failures (3061 targets) and LINKS into a
  43 MB hello-v8-jit.elf with 0 undefined symbols. The full AArch64 codegen /
  assembler / TurboFan / Sparkplug all compile clean for the Switch target --
  the toolchain + platform layer were solid enough that enabling JIT needed no
  compile-time changes. (Maglev left off initially to reduce surface.)
- **J1** (next): code-memory backend over libnx jit_* + wire CodeRange to
  allocate from the rx region; `RwxMemoryWriteScope`/`WritableJitAllocation`
  redirect via delta; cache flush on scope close. Code range for our config
  (arm64, no ptr-compression) is 256 MB max / 64 MB min -- start at the 64 MB
  minimum via `--code-range-size` since `jitCreate(JitType_CodeMemory)` maps the
  whole region twice (kernel code-memory limits unknown at 256 MB).
- **J2**: get a trivial function to tier up (Sparkplug) and execute correctly.
- **J3**: TurboFan/Maglev; relocation + IC patching correctness; GC of code.

### Runtime gap to close for J1 (what fails now)

The JIT engine links, but at runtime `CodeRange::InitReservation` ->
`SetPermissions(kReadWriteExecute)` will fail: our `mman-horizon.cc` `mprotect`
forces `Perm_Rw` (no exec) and `svcSetMemoryPermission` forbids `Perm_X`
anyway. ThreadIsolation is off (sandbox off), so V8 takes the plain
`SetPermissions(kReadWriteExecute)` path. We must instead serve the code range
from a libnx `jit_*` region and redirect writes by the rw/rx delta.

## Risks / unknowns

- Many code writes may bypass WritableJitAllocation in older spots; need to
  audit. (Recent V8 is mostly consolidated, which is why we pinned 15.0.243.)
- `armDCacheFlush`/`armICacheInvalidate` granularity + when exactly to flush
  (every scope close is safe but slow; optimize later).
- libnx jit region size limits; one big region vs many.
- Trusted/code pointer tables interplay with sandbox-off config.
