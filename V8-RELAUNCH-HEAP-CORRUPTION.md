# switch-v8: clean exit corrupts the hbloader heap → next homebrew launch crashes

**Owner:** ~~switch-v8 package~~ → **embedder + libuv self-pipe** (see RESOLUTION)
**Severity:** Blocker for relaunching homebrew after a socket-using V8 app
**Status:** RESOLVED — the real cause was a libuv self-pipe leak on the embedder
side, NOT the V8 mman arena. Fixed in nx.js by calling `uv_library_shutdown()`
before `socketExit()`.

## RESOLUTION (the actual root cause)

This report's mman-arena hypothesis was a **red herring**. The real cause:

libuv's async/signal self-wakeup "pipe" is a **loopback-TCP socket pair** on
Horizon (libnx has no anonymous pipes; switch-libuv's `horizon-port.c` emulates
`pipe()` with a socket pair). Those self-pipe fds are closed by
`uv_library_shutdown()`. The switch-libuv port deliberately disables the
`__attribute__((destructor))` that would call it at libc teardown (it runs
*after* `socketExit()` and faults), and **requires the embedder to call
`uv_library_shutdown()` explicitly while the socket layer is still up**.

nx.js wasn't doing that, so the self-pipe bsd sockets leaked and `socketExit()`
tore down the `bsdsocket` sysmodule with live sessions → it faulted on the NEXT
launch (the `bsdsocket + 0xe7064` User Break), and the dangling state also
showed up as the hbmenu malloc fault. Only socket-using apps create the
self-pipe (via the async watcher / threadpool), which is why a non-socket
hello-world relaunched fine.

Fix (nx.js `main.cc`): call `uv_library_shutdown()` immediately before
`plExit`/`romfsExit`/`socketExit`. Verified across multiple relaunches on
hardware. See the nx.js commit `e0244c5`.

> The switch-v8 15.0.243-5 mman teardown change (below) was made while chasing
> this and is KEPT as defensive teardown hygiene (it doesn't hurt), but it is
> NOT what fixed the crash.

---

## Original mman-arena investigation (red herring — kept for the record)

### Two-phase teardown + cache flush (switch-v8 15.0.243-5, defensive only)

Key facts established while investigating:
- The slab `src` blocks are `memalign`'d from the **shared hbloader heap**, and
  that heap **persists across NRO launches** (hbloader pre-allocates it via
  `svcSetHeapSize` and hands the SAME region to every child through the homebrew
  env — confirmed in libnx `__libnx_initheap`, `envHasHeapOverride()` path). That
  is exactly why the corruption survives relaunches and only a console restart
  (which re-grows the heap) clears it.
- While a slab `src` is `svcMapMemory`-aliased it is `MemType_WeirdMappedMem`.
- The aliasing itself is REQUIRED: V8 reserves a contiguous cage (`PROT_NONE`)
  then commits sub-ranges with `MAP_FIXED` at the exact reserved addresses, which
  raw `memalign` pointers can't satisfy — so we must alias heap blocks to those
  addresses. (libnx's own `jitCreate` aliases heap memory the same way.)

Most likely mechanism: the OLD teardown unmapped + `free()`d each slab in one
pass. Freeing one `src` back into newlib's allocator while an ADJACENT `src` was
still aliased (`WeirdMappedMem`) made newlib's free-list **coalescing** read a
still-aliased neighbor and corrupt the list — inherited by the next process.

Fix in `Arena::Teardown()`:
1. **Phase 1:** `armDCacheFlush` each slab's arena alias (V8 wrote through the
   `dst` alias) then `svcUnmapMemory` it — for ALL slabs first.
2. **Phase 2:** only after every alias is gone, `free()` the `src` blocks, so no
   coalesce can ever touch a still-mapped neighbor.

Verify with the relaunch repro below. If it STILL corrupts, the fallback is to
stop sourcing slabs from the shared heap at all (see "Suggested direction").

---

### Original root-cause analysis

## Symptom

1. Launch an nx.js (V8) app from hbmenu. It runs fine.
2. Exit cleanly with `+` (the app reaches full teardown, including
   `horizon_mman_teardown()` — verified; the loop breaks to teardown).
3. Launch ANY homebrew again (even the same app). It **crashes** — actually
   **hbmenu / hbloader crashes**, not the new app.
4. A full console restart clears it (heap is reinitialized).

So this is **persistent corruption of state that survives across NRO launches**
— i.e. the hbloader-owned heap/address space — caused by the FIRST app's run,
surfacing only when the next process reuses that heap.

## Crash evidence (symbolized)

Two Atmosphère crash reports each repro:

- `nx-hbmenu` — Data Abort, **Fault Address 0x0**, PC `nx-hbmenu + 0xf6b34`.
- `bsdsocket` (Program ID 0100000000000012) — **User Break** at `bsdsocket +
  0xe7064` (secondary symptom; the socket sysmodule session state is also bad).

Disassembling `hbmenu.nro` (raw aarch64) at the crash PC:

```
f6b00: ldr   x0, [x19, #8]          ; load free-block header (size|flags)
f6b04: and   x0, x0, #~3            ; mask off low flag bits
f6b08: cmp   x20, x0
f6b0c: sub   x0, x0, x20
f6b10: ccmp  x0, #0x1f, #4, ls
f6b14: b.le  0xf6e50
...
f6b20: add   x2, x19, x20           ; x2 = block + requested  (split point)
f6b28: str   x20, [x19, #8]
f6b30: str   x2, [x22, #16]
f6b34: str   x1, [x2, #8]           ; <-- FAULT: write block header at [x2+8]
```

This is **newlib `malloc`/`free` free-list block-splitting**. The fault means
hbmenu's allocator is walking a **corrupted free list** (block pointers/sizes
point at ~null). hbmenu didn't do anything wrong — its heap was already
corrupted before it started, by the previous V8 process.

## Root cause (mman arena aliasing)

`mman-horizon.cc` builds V8's data arena like this:

- Reserve address space in the **stack region**: `virtmemFindStack` +
  `virtmemAddReservation` (lines ~221–224).
- Back each 16 MiB slab by allocating a source block from the **normal heap**
  via `memalign(kPage, slab)` (line ~267) and **aliasing** it into the reserved
  arena address with `svcMapMemory(dst_arena_addr, src_heap_block, slab)`
  (line ~272).
- Teardown (`Arena::Teardown`, lines ~346–367): `svcUnmapMemory(arena_addr,
  src, slab)` then `free(src)` for each slab, then `virtmemRemoveReservation`.

The corruption is in this **`svcMapMemory` alias → `svcUnmapMemory` → `free(src)`**
lifecycle. The `memalign`'d `src` slabs are part of the hbloader heap. After the
map/unmap alias dance (and a full app run writing through the `dst` alias),
returning those slabs to the heap via `free(src)` leaves the heap's free-list
metadata inconsistent — so the next process (hbmenu) faults in malloc.

Likely culprits to investigate in `mman-horizon.cc`:
1. **Alias coherency:** memory written via the `dst` (arena) alias may not be
   coherent with the `src` (heap) virtual mapping at `svcUnmapMemory` time, or
   `svcUnmapMemory` leaves the `src` pages in a state newlib's allocator
   metadata doesn't expect when `free(src)` re-links them.
2. **Ordering:** `free(src)` after `svcUnmapMemory` may need a cache flush /
   different order, or the `src` blocks should NOT be returned to the shared
   heap at all (keep them, or allocate the slab sources from a dedicated
   region that is never freed back into the hbloader heap).
3. **Partial-unmap path** (the prefix/suffix re-map for large alignment,
   lines ~22–24 / the `munmap` path ~397–440): if a partial unmap leaves a
   slab partially aliased, `free(src)` corrupts the heap.

### Suggested direction

Don't `memalign` the slab sources from the shared (hbloader) heap and then
`free()` them back. Either:
- Allocate slab backing from a **dedicated, page-aligned region that is
  `svcUnmapMemory`'d and then released to the kernel directly (not `free()`d
  into the newlib heap)**, or
- Keep the source allocations but ensure the unmap fully restores the `src`
  pages (permissions + cache state) before `free()`, or simply **leak the slab
  sources** (don't `free` them) if that's safer than corrupting the heap —
  hbloader resets the heap on the next launch anyway... except the evidence
  shows it does NOT fully reset, so leaking may still strand address space.

A reliable repro/validation harness: the `hello-v8` JIT build (or nx.js) —
launch, exit with `+`, then launch any homebrew; it should NOT crash hbmenu.

## What the consumer (nx.js) already does correctly

- Reaches full teardown on clean `+` exit.
- Closes all libuv handles + their socket fds, then `socketExit`/`plExit`/
  `romfsExit`, then calls `horizon_mman_teardown()` as the LAST thing before
  `return` (after `iso->Dispose()` / `V8::Dispose()` / `V8::DisposePlatform()`).
- So `horizon_mman_teardown()` IS invoked on a clean exit; the corruption
  happens inside the arena map/unmap/free lifecycle itself, not from nx.js
  skipping teardown.

(For reference, the earlier separate finding — force-quit skips teardown and
leaks — is real too, but THIS report is specifically the clean-exit case, which
must not corrupt the heap.)
