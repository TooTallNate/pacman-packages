# V8 heap larger than ~32 MiB → Data Abort in the GC marking barrier

## STATUS: FIXED in switch-v8 15.0.243-9 (full JIT + jitless, both regimes)

The original report (full-JIT memory allocation crashing even in application
mode) was **two W^X bugs in V8's GC**, NOT the heap-commit/OOM theory the rest of
this doc was built around. Both are fixed; verified on hardware.

Final progression (forced full-JIT, application mode, `examples/heap-stress`):

| build | full-JIT app-mode result |
|---|---|
| pre-fix | Data Abort @ **32 MiB** — marking barrier |
| +0008 | Data Abort @ **3040 MiB** — GC sweeper |
| **+0009** | ✅ **graceful `RangeError` @ ~3045 MiB, SURVIVED** |

Jitless already passed (applet 169 MiB; application 3110 MiB) thanks to the -7
ENOMEM change; full JIT now matches it.

### Root cause (corrected): two code-page W^X write sites in the GC

Horizon executable memory is a libnx jit_* CodeMemory region with two aliases of
the same physical pages: **rx** (execute, read-only) and **rw** (writable = rx +
`horizon_jit_rw_delta`). V8 addresses code via rx and patch 0003 redirects
*instruction-stream* writes to rw. But two GC paths write **data** into the
`MemoryChunk`/free-space at the start of a CODE page via the rx alias → Data
Abort (fault address inside the code arena, only under full JIT):

1. **Patch 0008** — `MutablePage::SetOldGenerationPageFlags` →
   `RawSetTrustedAndUntrustedFlags` writes `Chunk()->untrusted_main_thread_flags_`
   (the in-page flags) during marking start/stop. Fixed by routing that store
   through the rw alias for code pages (no-op delta for data pages).
2. **Patch 0009** — `Sweeper::ZeroOrDiscardUnusedMemory` `memset()`s freed
   regions to zero "to help OS page compression". V8 guards it with
   `RwxMemoryWriteScope` when `is_executable()`, but that scope is a no-op on
   Horizon. The zeroing is a pure optimization (Horizon has no OS page
   compression), so we skip it for executable pages.

### How it was isolated (the decisive evidence)

- A forced-**jitless** `heap-stress` reached **3110 MiB** in the same
  application-mode regime where full-JIT died at 32 MiB → the variable is JIT,
  not memory.
- An instrumented mman (per-commit logging) showed the V8 **heap** committed only
  32 MiB with **zero commit failures**; the multi-GiB `Uint8Array` backing came
  from `malloc` (newlib heap), not the mman arena. The crash fault address was
  **inside the code arena** → a W^X write, not an OOM/commit gap.

### Note on `ArrayBuffer` backing stores

`ArrayBuffer::Allocator::NewDefaultAllocator()` uses `malloc`/`calloc` (newlib
heap), NOT the V8 mman DATA arena. So large TypedArray/ArrayBuffer workloads draw
from the newlib heap and are bounded by real physical memory, independent of the
V8 `max_heap` / `horizon_mman_data_arena_size()` budget. (The -7 ENOMEM change
still governs the V8 *object* heap in the arena.)

---

## (Earlier finding) full-JIT code-page W^X — root cause detail

**Corrected diagnosis (supersedes the "commit not surfaced as OOM" theory below).**
On-device diagnostics (instrumented mman + a forced-jitless `examples/heap-stress`)
proved the original commit-contract theory WRONG and isolated the real cause:

- The crash is a Data Abort writing the **`MemoryChunk` header flags of a
  CODE-space page** (`MutablePage::SetOldGenerationPageFlags` →
  `RawSetTrustedAndUntrustedFlags` → `Chunk()->untrusted_main_thread_flags_ = …`)
  during GC marking start/stop (`Activate/DeactivateSpaces`, marking-barrier.cc).
- The faulting address is **inside the libnx jit_* code arena** (confirmed: fault
  `0x3b21200000` vs logged `code arena rx=0x3b211d4000`, offset +0x2c000). That
  alias is **rx (execute, NOT writable)** — Horizon W^X. The generic heap
  flag-write path does NOT go through the `WritableJitAllocation` rw-alias
  redirect (patch 0003), so it writes straight to the read-only alias → fault.
- It is **JIT-specific**, not memory-related. Decisive cross-check, same
  application-mode regime / same RAM:
  - full JIT → crashed after only **32 MiB** committed (mman log: **zero** commit
    failures — memory was fine).
  - **jitless → reached 3110 MiB**, caught a graceful `RangeError`, `Array.join`
    OK, **SURVIVED**.
- Applet jitless also PASSED (169 MiB, graceful RangeError) — the wall there is
  real physical memory (~137 MiB), handled cleanly by the -7 ENOMEM change.

**So:** the -7 `mprotect→ENOMEM` change is correct and load-bearing for the
*jitless* / data-arena OOM path (keep it), but the headline crash is a separate
**W^X bug in the full-JIT code-page flag write**. Fix target: route
`Chunk()->untrusted_main_thread_flags_` (and the trusted/untrusted raw flag
writes) through the rw alias (`horizon_jit_rw_delta`) when the chunk is a code
page — i.e. give `RawSetTrustedAndUntrustedFlags` the same treatment V8 already
gives `SetFlagMaybeExecutable` (which wraps the write in `RwxMemoryWriteScope`
when `is_executable()`), but using Horizon's alias redirect instead of mprotect.

---

## (Superseded) earlier status: jitless OK, full-JIT faults — commit theory

On-hardware results (standalone `examples/heap-stress` NRO linked against -7,
which incrementally allocates 1 MiB `Uint8Array`s touching every page, then does
a large `Array.prototype.join`):

| regime | JIT | result |
|---|---|---|
| **applet** (NRO, ~137 MiB real free) | jitless | ✅ **PASS** — workload A stopped at 169 MiB with a **caught `RangeError: Array buffer allocation failed`**, `Array.join` (~64 MiB) completed, app survived. No Data Abort. |
| **application** (title-redirect) | full JIT | ❌ **FAIL** — Data Abort @ 0x0 in `MutablePage::SetOldGenerationPageFlags` ← `DeactivateSpaces` (marking-barrier.cc) after committing only ~32 MiB. |

So the -7 `mprotect`→`ENOMEM` change **fixed the jitless path but not full-JIT.**
The full-JIT crash is the *same* fault as the original report (page-flag write
into an unbacked page during GC marking), meaning the failing commit on the JIT
heap path is reaching V8 as *success* — it does NOT go through the `mprotect`
return that -7 now checks. Diagnosis continues below (see "Full-JIT path: still
open").

### Two confounders observed in the application-mode run
- **`horizon_mman_data_arena_size()` reports ADDRESS SPACE, not committable RAM.**
  In both regimes it returned **1024 MiB** (the `virtmemFindStack` first-fit), and
  the app sized the heap to 682 MiB — but applet mode only has ~137 MiB of real
  free memory, and workload A correctly hit the wall at 169 MiB. **Do not size a
  heap from this value alone**; it is the address-space ceiling, not backable
  memory. (See "What the nx.js embedder should now do" — must also clamp by real
  free memory.)
- **Title-redirect "application mode" is NOT ~3 GiB of usable RAM.** The run
  logged `total=3189 MiB used=3185 MiB` — the host game already consumed nearly
  all physical memory, so only a few MiB are actually committable despite the
  large grant. That is why full-JIT died at ~32 MiB: `svcMapMemory` ran out of
  real pages almost immediately (compounding the unfixed full-JIT commit bug).

---

## STATUS (original -7 change, still valid for jitless): commit failure surfaced

Two changes in `horizon-src/mman-horizon.cc`:

1. **Commit failure is now surfaced to V8 (fixes the Data Abort).** `Arena::SetPerm`
   now returns `bool`, and `mprotect` / `mmap(MAP_FIXED)` / `mmap(RW)` return
   `-1` / `MAP_FAILED` with **`errno = ENOMEM`** when a slab cannot be committed
   (memalign or `svcMapMemory` failure). V8's `base::OS::SetPermissions`
   (Horizon reuses `platform-posix.cc`) checks the `mprotect` return and
   *requires* `errno==ENOMEM` on failure (else it `CHECK`-aborts), so this routes
   V8 into its normal **commit-failed → GC harder → catchable RangeError /
   graceful FatalOOM** path (`MemoryAllocator::AllocateUninitializedChunkAt`
   returns `{}` on a null base) instead of writing page-flag metadata into
   unbacked pages → Data Abort @ 0x0. **An over-large heap is no longer fatal;
   it degrades gracefully.**

2. **The real ceiling is exposed (`horizon_mman_data_arena_size()`).** The DATA
   arena is carved from the **STACK virtmem region** via `virtmemFindStack`
   (first-fit over {1 GiB, 512, 256, 128, 64 MiB}) — it is NOT the process
   memory grant. *This is why "3.5 GiB free" does not yield a big heap:* the V8
   heap + ArrayBuffers all live in this bounded reservation. The new
   `extern "C" size_t horizon_mman_data_arena_size(void)` returns the bytes
   actually reserved (forcing arena init) so the embedder can size the heap to
   fit.

### What the nx.js embedder should now do (`source/main.cc`)

The heap max must fit BOTH the address-space arena AND the real committable
memory. `horizon_mman_data_arena_size()` gives the former (1 GiB); the latter is
`TotalMemorySize - UsedMemorySize` (the *true* free RAM — yes, `Used` is the
whole grant in app/title-redirect mode, so this is small there; that is correct,
because the host game really has eaten the RAM). Budget from the **min** of the
two:

```c
extern size_t horizon_mman_data_arena_size(void);   // from switch-v8

u64 total = 0, used = 0;
svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
svcGetInfo(&used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
size_t free_ram = total > used ? (size_t)(total - used) : 0;   // real backable

size_t arena    = horizon_mman_data_arena_size();              // address space
size_t ceiling  = free_ram < arena ? free_ram : arena;         // min of the two
// Headroom for ArrayBuffer backing stores, slab bookkeeping, code arena (JIT),
// libuv/Skia/native allocs — all draw from the SAME memory. Reserve generously.
size_t reserve  = ceiling / 3;
size_t max_heap = ceiling > reserve ? ceiling - reserve : 0;
// clamp to sane bounds (e.g. >=16 MiB, <= a few hundred MiB), then:
create_params.constraints.ConfigureDefaultsFromHeapSize(8u<<20, max_heap);
```

Notes:
- The `arena` value is the address-space ceiling (good for not over-reserving),
  but **real free memory is the binding constraint** — in the app-mode test the
  arena was 1 GiB yet only ~137 MiB was actually committable, so a 682 MiB heap
  was nonsense. Always take the `min`.
- ArrayBuffers/TypedArrays are `memalign`'d from the SAME arena, so the heap max
  must leave room for them. Large-ArrayBuffer apps want a smaller heap fraction.
- **WARNING (current -7 state): the `min`-clamp above keeps you stable, but if an
  app genuinely needs more memory than is committable, the JITLESS path now OOMs
  gracefully while the FULL-JIT path can still Data-Abort** (see "Full-JIT path:
  still open"). Until that is fixed, prefer conservative heap sizing under full
  JIT, or run jitless when memory is tight.

### Full-JIT path: still open (the remaining bug)

The -7 fix made `mprotect`/`mmap` return `ENOMEM` on commit failure, which V8's
`base::OS::SetPermissions` honors → graceful OOM. That works for **jitless**.
Under **full JIT** the same `Uint8Array` workload still Data-Aborts in
`SetOldGenerationPageFlags` ← `DeactivateSpaces`, i.e. a failed commit is still
reaching V8 as success on the JIT heap path. Leads to chase:

- The heap chunk commit goes `MemoryAllocator::AllocateAlignedMemory` →
  `VirtualMemory(page_allocator, …, kReadWrite)` → `page_allocator->AllocatePages`.
  With a code range present (JIT), V8 may route heap allocations through a
  `BoundedPageAllocator` over a pre-reserved cage; the OS-level commit
  (mprotect/SetPermissions) happens inside that allocator. **Verify whether that
  commit's failure return is propagated, or whether `BoundedPageAllocator`
  assumes the cage is already committed** (in which case our lazy-slab arena
  never gets an mprotect for those pages and they fault later in GC).
- `OS::HasLazyCommits()` returns **false** on Horizon (only `V8_OS_HORIZON`+
  `V8_OS_POSIX` are defined, not `V8_OS_LINUX`), so V8 *should* commit eagerly.
  Confirm the JIT path actually does, and isn't relying on a
  `RecommitPages`/`SetPermissions`-skipped fast path.
- Possible fixes: (a) propagate the `AllocatePages`/`SetPermissions(RW)` failure
  on the JIT cage the same way `mprotect` now does; or (b) eagerly fault-in
  (commit) each chunk's metadata/header page at allocation so the marking barrier
  never writes into an unbacked page; or (c) ensure the lazy-slab `CommitRange`
  actually covers the chunk header page for JIT-cage allocations.

### Verify on device (re-run `examples/heap-stress` after a fix)
- Applet (jitless): already PASSES — graceful `RangeError`, `Array.join` ok.
- Application (full JIT): currently FAILS — must reach the same graceful outcome
  (catchable `RangeError`/FatalOOM), **never** Data Abort @ 0x0 in
  `SetOldGenerationPageFlags`.
- Log `horizon_mman_data_arena_size()`, `free_ram`, and the chosen `max_heap`.

---

## TL;DR

On Horizon, configuring V8 with anything much above a ~32 MiB max heap makes V8
**crash with a Data Abort (write to an unmapped page) inside the GC marking
barrier** — even when only a few MiB of objects have actually been allocated.
The fault is `v8::internal::MutablePage::SetOldGenerationPageFlags` called from
`ActivateSpaces` (incremental marking start). The underlying cause appears to be
that the `mman-horizon.cc` DATA arena's lazy slab commit (`Arena::CommitRange`
→ `svcMapMemory`) can fail / not cover the pages V8 believes are part of its
heap, and that failure is **not surfaced to V8 as an allocation failure** — so
V8 walks page metadata it thinks it owns and writes into PROT_NONE / unmapped
address space.

Net effect for the embedder (nx.js): we are stuck choosing between
- a **tiny heap (~32 MiB)** that is stable but fatally OOMs
  (`CALL_AND_RETRY_LAST`) on any memory-heavy JS (e.g. building an NSP — a big
  `Array.prototype.join`), or
- a **larger heap** that Data-Aborts in the GC as soon as marking activates.

There is currently **no heap size that is both large enough for real workloads
and stable**. This needs a fix in the port (the mman/V8 reservation contract),
not just an embedder constant.

## Symptom A — too-small heap → FatalOOM (the original report)

Released beta runs (heap effectively floored to ~32 MiB, see "Why 32 MiB" below)
crash when an app builds an NSP in JS:

```
PC  -> v8::base::OS::Abort()
LR  -> v8::base::FatalNoSecurityImpact(...)   // "[FatalOOM] JavaScript OOM: CALL_AND_RETRY_LAST"
      -> Builtins_ArrayPrototypeJoinImpl       // Array.prototype.join() of a huge array
        -> Builtins_ArrayMap
          -> ... (JS) -> main event loop
```

i.e. a genuine JS-heap exhaustion: `Array.join` couldn't allocate the result
string within the small heap, and V8 aborts (this OOM path is not catchable from
JS).

## Symptom B — larger heap → Data Abort in GC marking

Raising the embedder's `ResourceConstraints` max heap (tried 192 MiB, 512 MiB,
and 1.5 GiB) does NOT fix it — instead V8 crashes almost immediately, while
allocating only a few MiB of `Uint8Array`:

```
Exception: Data Abort, Fault Address 0x0
PC  -> v8::internal::MutablePage::SetOldGenerationPageFlags(MarkingMode)
LR  -> v8::internal::(anonymous)::ActivateSpaces(Heap*, MarkingMode)   // marking-barrier.cc
      -> Builtins_CreateTypedArray            // new Uint8Array(1 MiB)
        -> Builtins_TypedArrayConstructor
          -> ... (JS) -> Invoke -> main
```

Reproduced deterministically: a probe that allocates 1 MiB `Uint8Array`s one at
a time (touching every page) crashes before the first 8 MiB checkpoint. The
larger the configured heap, the more old-generation page-flag metadata marking
touches, so the unbacked-page write happens almost at once.

All Data-Abort crashes have `Fault Address: 0x0000000000000000` and PC in
`SetOldGenerationPageFlags` / `marking-barrier.cc`.

## Why 32 MiB ("application mode" memory probe)

The embedder sizes the heap from `svcGetInfo`:

```
[v8] mem_total=3189 MiB free=3 MiB regime=application -> mode=jit
```

`InfoType_UsedMemorySize` reports the **entire grant as used immediately**, so
`free = total - used ≈ 3 MiB` even with a 3 GiB grant. The old budget did
`max_heap = mem_free - reserve`, which underflowed to the 32 MiB floor. That is
why released builds effectively ran a ~32 MiB heap in application mode and hit
Symptom A. (We can fix the probe to budget from `mem_total` instead — but that
just turns Symptom A into Symptom B, which is the real blocker.)

## Where the contract breaks (mman-horizon.cc)

`Arena` reserves a DATA arena from the STACK region, trying
`{1 GiB, 512, 256, 128, 64 MiB}` and taking the first `virtmemFindStack` /
`virtmemAddReservation` that succeeds (`Arena::EnsureInit`). It then commits
**16 MiB slabs lazily** on first touch:

```cpp
// Arena::CommitRange(addr, size): for each 16 MiB slab in range,
//   memalign a block and svcMapMemory it into the reserved arena.
//   On svcMapMemory failure it DiagLogs "DATA slab svcMapMemory FAILED" and
//   returns false.
```

The problem is the **failure path**: when a slab can't be committed (arena
address-space OOM, or the kernel won't satisfy `svcMapMemory`), `CommitRange`
returns false, but V8's heap/GC has already been told (via the
`ResourceConstraints` max heap and the page reservation) that those pages are
usable. V8's incremental marking then writes page flags into pages that were
never backed → Data Abort at `0x0`.

Two things to investigate / fix in the port so the embedder can use a larger,
stable heap:

1. **Make commit failure visible to V8 as an allocation failure, not a fault.**
   V8's `PageAllocator` / `VirtualMemory` commit (`SetPermissions` → mprotect →
   `CommitRange`) must propagate the `CommitRange == false` result up as a
   failed commit so V8 takes its normal "allocation failed → GC harder → throw
   RangeError / graceful OOM" path instead of writing into the page. Today a
   failed/lazy commit appears to be silently treated as success (or the page is
   PROT_NONE and only faults on first GC write).

2. **Reserve enough committable DATA arena to match the heap V8 is configured
   with**, OR cap/communicate the real arena size to the embedder. If the arena
   can only reserve 128 MiB from the STACK region, V8 must be configured with a
   max heap that fits inside it (minus slabs needed for code/other). Right now
   the arena size (`{1GiB..64MiB}` first-fit) and the embedder's heap max are
   chosen independently, so they disagree.

It would help to expose the actually-reserved `arena_size_` (and maybe a
"max committable" number) to the embedder so it can call
`ConfigureDefaultsFromHeapSize` with a value that the mman can truly back.

## What the embedder needs from the port

A way to set a V8 max-heap that is **both** large enough for memory-heavy JS
(hundreds of MiB) **and** guaranteed mappable — i.e. either:
- a larger, reliably-committable DATA arena in application mode (3 GiB grant
  should comfortably allow e.g. a 512 MiB+ JS heap), and/or
- commit-failure surfaced to V8 so an over-large heap degrades to a catchable
  `RangeError`/FatalOOM instead of a Data Abort.

Once that exists, nx.js will budget the heap from `mem_total` per regime and
verify a multi-hundred-MiB allocation + a large `Array.join` on device.

## Reference

- Embedder heap config: `nx.js` `source/main.cc`, `ConfigureDefaultsFromHeapSize(8 MiB, max)`
  + `set_code_range_size_in_bytes(64 MiB)` (JIT) under
  `--single-threaded --single-threaded-gc --predictable`.
- Crash faulting frames (V8 15.0.243): `src/heap/marking-barrier.cc`
  (`ActivateSpaces`), `MutablePage::SetOldGenerationPageFlags`.
- mman: `switch/v8/horizon-src/mman-horizon.cc` — `Arena::EnsureInit` (reservation
  first-fit), `Arena::CommitRange` (lazy 16 MiB slab `svcMapMemory`, returns
  false on failure), `CodeArena` (separate jitCreate dual-map region).
- Memory probe quirk: `InfoType_UsedMemorySize` ≈ full grant, so
  `total - used` is misleading; budget from `InfoType_TotalMemorySize`.
- GN: built `v8_lower_limits_mode=true` (16 MiB JSDispatchTable) — unrelated to
  this, but note the heap-related lower-limits reductions when sizing.
