# V8 heap larger than ~32 MiB → Data Abort in the GC marking barrier (mman commit not surfaced as OOM)

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
