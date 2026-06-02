// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
//
// Faithful <sys/mman.h> for Nintendo Switch (Horizon / devkitA64 / libnx).
//
// newlib has no mmap and Horizon has no demand-paged anonymous mmap. We build
// one over libnx primitives, the same way libnx maps thread stacks
// (nx/source/kernel/thread.c):
//   * reserve a slice of the STACK region (the region svcMapMemory may target)
//     via virtmemFindStack + virtmemAddReservation (address space only),
//   * each mmap() allocates ONE contiguous heap block (memalign) and aliases it
//     into the reserved arena with a SINGLE svcMapMemory call.
//
// IMPORTANT: Horizon limits the number of distinct kernel memory blocks. An
// earlier per-PAGE mapping design exhausted that resource (svcMapMemory ->
// 0xce01 ResourceExhausted after ~28 MB / ~7000 pages). So we map per
// allocation, NOT per page, keeping the live-mapping count proportional to the
// number of allocations.
//
// Partial unmap (V8's PageAllocator over-allocates then frees the unaligned
// prefix/suffix to satisfy large alignment): we svcUnmapMemory the whole
// affected region, then re-map the surviving prefix/suffix AT THEIR ORIGINAL
// ADDRESSES using sub-slices of the original heap backing (which is contiguous,
// so backing offset = keep_start - region_start). This preserves the addresses
// V8 keeps using.
//
// mprotect maps onto svcSetMemoryPermission (None/R/RW). Executable memory is
// the libnx jit_* path, not here.

#include "src/base/platform/horizon/sys/mman.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <switch.h>

namespace {

constexpr size_t kPage = 0x1000;

inline size_t RoundUpPage(size_t n) { return (n + kPage - 1) & ~(kPage - 1); }
inline uintptr_t RoundUpPageAddr(uintptr_t a) {
  return (a + kPage - 1) & ~(uintptr_t{kPage} - 1);
}

static void DiagLog(const char* fmt, ...) {
  FILE* f = fopen("sdmc:/hello-v8-mman.log", "a");
  if (f == nullptr) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fclose(f);
}

// ---------------------------------------------------------------------------
// Executable code arena (full JIT).
//
// Horizon enforces W^X, so executable memory comes from a libnx jit_*
// (JitType_CodeMemory) region that exposes TWO permanently-mapped aliases of
// the same physical pages: rx (executable) and rw (writable). V8 uses the rx
// address everywhere; code WRITES are redirected to rw = rx + g_jit_delta (see
// horizon_jit_rw_delta below, consumed by WritableJitAllocation).
//
// mmap(MAP_JIT) bump-allocates rx address space from this region. mprotect with
// PROT_EXEC is a no-op (the rx alias is already executable; the rw alias is
// already writable). One jitCreate region for the whole V8 code range.
// ---------------------------------------------------------------------------
// Runtime-tunable code-arena budget (set by the embedder via
// horizon_mman_set_code_budget BEFORE V8 init; see the extern "C" API below for
// full docs). Defaults preserve the original behavior (64 MiB WASM headroom,
// automatic total/3 ceiling).
static size_t g_wasm_headroom_mb = 64;
static size_t g_max_code_mb = 0;

class CodeArena {
 public:
  static constexpr size_t kCodeFloor = size_t{64} << 20;  // V8's min code range

  bool EnsureInit(size_t need) {
    if (initialized_) return rx_base_ != 0;
    initialized_ = true;

    // Desired size: cover V8's code-range request (floored at the 64 MiB V8
    // minimum) PLUS headroom for WebAssembly's separate code space. WASM
    // reserves its own region (builtin jump tables + function code) from this
    // same arena; without the headroom the JS reservation consumes everything
    // and WASM OOMs ("Allocate initial wasm code space"). The headroom is
    // embedder-tunable (g_wasm_headroom_mb): non-WASM apps set it to 0.
    size_t want = (need < kCodeFloor ? kCodeFloor : need) +
                  (g_wasm_headroom_mb << 20);

    // Budget guard: jitCreate maps the region TWICE (rx + rw aliases), so a
    // size-S arena costs 2*S of real memory, and the V8 heap + data arena also
    // need room. Cap the arena at ~1/3 of the process's TOTAL memory grant from
    // svcGetInfo(InfoType_TotalMemorySize). This is a CEILING, not a target: it
    // protects constrained modes (in applet mode total ~= 381 MiB, so the cap
    // ~= 127 MiB lightly clamps the 128 MiB we want) without growing the arena
    // when more memory is available. NB: we cap on TOTAL, not (total - used):
    // in full-memory mode `used` already counts V8's big heap reservation, so
    // available reads as only a few MiB even with ~3 GiB total — capping on
    // `used` would wrongly starve the arena. Floor is V8's 64 MiB minimum code
    // range; a failed jitCreate is handled by the retry loop below.
    u64 total = 0;
    if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize,
                               CUR_PROCESS_HANDLE, 0)) &&
        total != 0) {
      size_t cap = static_cast<size_t>(total) / 3;
      if (cap < kCodeFloor) cap = kCodeFloor;
      if (want > cap) want = cap;
    }

    // Explicit embedder ceiling (g_max_code_mb), if set, overrides downward.
    if (g_max_code_mb != 0) {
      size_t hard = g_max_code_mb << 20;
      if (hard < kCodeFloor) hard = kCodeFloor;  // never below V8's minimum
      if (want > hard) want = hard;
    }

    size_t sz = (want + 0xFFFFF) & ~size_t{0xFFFFF};
    Result rc = jitCreate(&jit_, sz);
    // If the budget-derived size fails to map, step down toward the floor.
    while (R_FAILED(rc) && sz > kCodeFloor) {
      size_t half = sz / 2;
      sz = (half < kCodeFloor) ? kCodeFloor : half;
      DiagLog("mman: jitCreate retry at 0x%zx\n", sz);
      rc = jitCreate(&jit_, sz);
    }
    if (R_FAILED(rc)) {
      DiagLog("mman: jitCreate(0x%zx) FAILED rc=0x%x\n", sz, rc);
      return false;
    }
    // Make the region executable once; for JitType_CodeMemory both aliases stay
    // mapped permanently and this just does the initial cache sync.
    jitTransitionToExecutable(&jit_);
    rx_base_ = reinterpret_cast<uintptr_t>(jitGetRxAddr(&jit_));
    uintptr_t rw_base = reinterpret_cast<uintptr_t>(jitGetRwAddr(&jit_));
    delta_ = rw_base - rx_base_;  // rw = rx + delta
    size_ = sz;
    next_ = rx_base_;
    // Hardening: do NOT log the writable (rw) alias base or the rw-rx delta.
    // The rx alias is execute-only and the rw alias is the only writable view of
    // generated code; leaking its address/delta to the SD log would hand an
    // attacker the one piece of info needed to locate the writable code mirror.
    DiagLog("mman: code arena rx=%p size=0x%zx type=%d\n", (void*)rx_base_, sz,
            jit_.type);
    return true;
  }

  void* Allocate(size_t size, size_t alignment) {
    size = RoundUpPage(size);
    if (alignment < kPage) alignment = kPage;
    if (!EnsureInit(size)) return nullptr;
    uintptr_t at = (next_ + alignment - 1) & ~(alignment - 1);
    if (at + size > rx_base_ + size_) {
      DiagLog("mman: code arena OOM (need 0x%zx at %p, end %p)\n", size,
              (void*)at, (void*)(rx_base_ + size_));
      return nullptr;
    }
    next_ = at + size;
    return reinterpret_cast<void*>(at);  // rx address
  }

  bool Contains(uintptr_t a) const {
    return rx_base_ != 0 && a >= rx_base_ && a < rx_base_ + size_;
  }
  intptr_t delta() const { return delta_; }

  // Sync caches after a batch of writes through the rw alias. `rx` is the code
  // (execute) address; the rw write target is rx + delta_.
  void Sync(uintptr_t rx, size_t n) {
    if (!Contains(rx)) return;
    armDCacheFlush(reinterpret_cast<void*>(rx + delta_), n);
    armICacheInvalidate(reinterpret_cast<void*>(rx), n);
  }

  void Teardown() {
    if (rx_base_ == 0) return;
    jitClose(&jit_);
    rx_base_ = 0;
    size_ = 0;
    next_ = 0;
    delta_ = 0;
    initialized_ = false;
  }

 private:
  bool initialized_ = false;
  Jit jit_{};
  uintptr_t rx_base_ = 0;
  uintptr_t next_ = 0;
  size_t size_ = 0;
  intptr_t delta_ = 0;
};

CodeArena g_code;

// A single committed mapping: heap block `src` aliased at [dst, dst+size).
struct Region {
  uintptr_t dst;
  void* src;
  size_t size;
  Region* next;
};

class Arena {
 public:
  bool EnsureInit() {
    if (initialized_) return base_ != 0;
    initialized_ = true;
    static const size_t kTry[] = {size_t{1} << 30, size_t{512} << 20,
                                  size_t{256} << 20, size_t{128} << 20,
                                  size_t{64} << 20};
    void* slice = nullptr;
    for (size_t s : kTry) {
      virtmemLock();
      slice = virtmemFindStack(s, kPage);
      if (slice) reservation_ = virtmemAddReservation(slice, s);
      virtmemUnlock();
      if (slice && reservation_) {
        arena_size_ = s;
        break;
      }
      slice = nullptr;
    }
    if (!slice) {
      DiagLog("mman: virtmemFindStack failed\n");
      return false;
    }
    base_ = reinterpret_cast<uintptr_t>(slice);
    next_free_ = base_;
    num_slabs_ = (arena_size_ + kSlab - 1) / kSlab;
    slab_src_ = static_cast<void**>(calloc(num_slabs_, sizeof(void*)));
    if (!slab_src_) {
      base_ = 0;
      return false;
    }
    DiagLog("mman: arena base=%p size=0x%zx slabs=%zu\n", slice, arena_size_,
            num_slabs_);
    return true;
  }

  bool Contains(uintptr_t a, size_t len) const {
    return a >= base_ && (a + len) <= base_ + arena_size_;
  }

  // Commit the slabs covering [addr, addr+size) on demand (lazy commit). Only
  // slabs actually touched get backed by svcMapMemory -> a PROT_NONE reservation
  // costs no committed memory. One svcMapMemory per 16 MiB slab keeps the kernel
  // mapping count tiny (vs per-allocation, which exhausted the block limit).
  bool CommitRange(uintptr_t addr, size_t size) {
    if (size == 0) return true;
    uintptr_t end = addr + size;
    size_t first = (addr - base_) / kSlab;
    size_t last = (end - 1 - base_) / kSlab;
    for (size_t i = first; i <= last && i < num_slabs_; i++) {
      if (slab_src_[i] != nullptr) continue;  // already committed
      uintptr_t slab_addr = base_ + i * kSlab;
      size_t slab = kSlab;
      if (slab_addr + slab > base_ + arena_size_) {
        slab = (base_ + arena_size_) - slab_addr;
      }
      void* src = memalign(kPage, slab);
      if (!src) {
        DiagLog("mman: DATA slab memalign(0x%zx) failed\n", slab);
        return false;
      }
      Result rc = svcMapMemory(reinterpret_cast<void*>(slab_addr), src, slab);
      if (R_FAILED(rc)) {
        DiagLog("mman: DATA slab svcMapMemory FAILED rc=0x%x at=%p size=0x%zx "
                "(slab %zu)\n", rc, (void*)slab_addr, slab, i);
        free(src);
        return false;
      }
      std::memset(reinterpret_cast<void*>(slab_addr), 0, slab);
      slab_src_[i] = src;
      if (++slab_count_ <= 6 || (slab_count_ % 8) == 0) {
        DiagLog("mman: DATA slab #%u (idx %zu) committed\n", slab_count_, i);
      }
    }
    return true;
  }

  // Reserve `size` bytes of address space WITHOUT committing (lazy). Commit
  // happens on first write access via CommitRange (from mprotect RW / a
  // read-write mmap).
  void* Reserve(size_t size) {
    size = RoundUpPage(size);
    if (!EnsureInit()) return nullptr;
    uintptr_t at = RoundUpPageAddr(next_free_);
    if (at + size > base_ + arena_size_) {
      DiagLog("mman: DATA arena address-space OOM: need 0x%zx, reserved %zu of "
              "%zu MB\n", size, (size_t)((next_free_ - base_) >> 20),
              (size_t)(arena_size_ >> 20));
      return nullptr;
    }
    next_free_ = at + size;
    return reinterpret_cast<void*>(at);
  }

  // Reserve + immediately commit (for read-write mmap).
  void* Allocate(size_t size) {
    void* p = Reserve(size);
    if (!p) return nullptr;
    if (!CommitRange(reinterpret_cast<uintptr_t>(p), RoundUpPage(size))) {
      return nullptr;
    }
    return p;
  }

  // Decommit is a no-op: slabs stay mapped until Teardown. V8's alignment-trim
  // frees (munmap of prefix/suffix) just leave committed-but-unused pages; the
  // bump allocator never reclaims anyway. This avoids per-free svcUnmapMemory
  // (mapping churn / kernel block exhaustion) and the survivor-remap dance.
  void Decommit(uintptr_t addr, size_t size) {
    (void)addr;
    (void)size;
  }

  // Make [addr,size) usable. With lazy commit, V8 calls SetPermissions(RW) on
  // pages it reserved (PROT_NONE) -> we COMMIT them here.
  //
  // NOTE: we do NOT downgrade committed pages to read-only / no-access. We tried
  // svcSetMemoryPermission(R/None) here as a hardening step, but the kernel
  // rejects it with 0xd401 (InvalidMemState) for our svcMapMemory-backed slab
  // aliases — homebrew cannot re-protect this memory (the same restriction that
  // blocks the in-place W^X flip; see jitflip-poc / PORTING-NOTES). So data
  // pages stay RW once committed. (The code arena's W^X is enforced structurally
  // by the JitType_CodeMemory rx/rw alias split, independent of this path.)
  void SetPerm(uintptr_t addr, size_t size, u32 perm) {
    if (base_ == 0 || !Contains(addr, size)) return;
    if (perm != Perm_None) {
      // Committing (RW / R). Ensure the slabs are backed.
      CommitRange(addr, RoundUpPage(size));
    }
  }

  // Unmap ALL slabs + release the address-space reservation. Required before the
  // .nro returns to hbloader/hbmenu: libnx's exit does NOT unmap manual
  // svcMapMemory regions, so leaked aliases corrupt the next process.
  void Teardown() {
    if (base_ == 0) return;
    for (size_t i = 0; i < num_slabs_; i++) {
      if (slab_src_[i] == nullptr) continue;
      uintptr_t slab_addr = base_ + i * kSlab;
      size_t slab = kSlab;
      if (slab_addr + slab > base_ + arena_size_) {
        slab = (base_ + arena_size_) - slab_addr;
      }
      svcUnmapMemory(reinterpret_cast<void*>(slab_addr), slab_src_[i], slab);
      free(slab_src_[i]);
      slab_src_[i] = nullptr;
    }
    free(slab_src_);
    slab_src_ = nullptr;
    if (reservation_ != nullptr) {
      virtmemLock();
      virtmemRemoveReservation(reservation_);
      virtmemUnlock();
      reservation_ = nullptr;
    }
    base_ = 0;
    next_free_ = 0;
    arena_size_ = 0;
    num_slabs_ = 0;
    initialized_ = false;
  }

 private:
  static constexpr size_t kSlab = size_t{16} << 20;  // 16 MiB commit slabs
  bool initialized_ = false;
  uintptr_t base_ = 0;
  uintptr_t next_free_ = 0;  // reserved (address-space) watermark
  size_t arena_size_ = 0;
  size_t num_slabs_ = 0;
  void** slab_src_ = nullptr;  // per-slab heap backing (nullptr = uncommitted)
  VirtmemReservation* reservation_ = nullptr;
  unsigned slab_count_ = 0;
};

Arena g_arena;
Mutex g_mman_mutex;

struct Lock {
  Lock() { mutexLock(&g_mman_mutex); }
  ~Lock() { mutexUnlock(&g_mman_mutex); }
};

}  // namespace

extern "C" {

void* mmap(void* addr, size_t length, int prot, int flags, int fd,
           off_t offset) {
  (void)fd;
  (void)offset;
  if (!(flags & MAP_ANONYMOUS) || length == 0) return MAP_FAILED;

  Lock lk;
  uintptr_t want = reinterpret_cast<uintptr_t>(addr);

  // Executable (JIT) memory: serve rx addresses from the libnx jit_* code
  // arena. V8 reserves the code range non-fixed (MAP_JIT, kNoAccessWillJitLater)
  // then mprotects it executable; both are handled here / in mprotect.
  if ((flags & MAP_JIT) && !(flags & MAP_FIXED)) {
    void* p = g_code.Allocate(length, kPage);
    return p ? p : MAP_FAILED;
  }

  if (flags & MAP_FIXED) {
    if (!g_arena.Contains(want, RoundUpPage(length))) {
      if (g_code.Contains(want)) return addr;  // code arena rx addr V8 reuses
      return MAP_FAILED;
    }
    // MAP_FIXED at an in-arena addr: PROT_NONE = decommit-in-place (no-op, keep
    // slabs); RW = (re)commit the slabs covering this range.
    if (prot != PROT_NONE) g_arena.CommitRange(want, RoundUpPage(length));
    return addr;
  }

  // Lazy commit: PROT_NONE reservations only reserve address space (no backing,
  // so V8's large cage/will-jit reservations cost no committed memory).
  // Read/write mappings reserve + commit immediately.
  void* result =
      (prot == PROT_NONE) ? g_arena.Reserve(length) : g_arena.Allocate(length);
  if (result == nullptr) return MAP_FAILED;
  return result;
}

int munmap(void* addr, size_t length) {
  if (addr == nullptr || addr == MAP_FAILED) return -1;
  Lock lk;
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  // Code arena addresses are bump-allocated and freed wholesale at teardown;
  // individual munmap of code pages is a no-op (V8 reuses the code range).
  if (g_code.Contains(a)) return 0;
  g_arena.Decommit(a, length);
  return 0;
}

int mprotect(void* addr, size_t length, int prot) {
  Lock lk;
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  // Code arena: rx alias is permanently executable, rw alias permanently
  // writable. Permission changes (incl. kReadWriteExecute) are no-ops here;
  // W^X is handled by the dual aliases + cache sync, not page permissions.
  if (g_code.Contains(a)) return 0;
  u32 perm = (prot == PROT_NONE) ? Perm_None
             : (prot & PROT_WRITE) ? Perm_Rw
                                   : Perm_R;
  g_arena.SetPerm(a, length, perm);
  return 0;
}

int madvise(void* addr, size_t length, int advice) {
  // MADV_DONTNEED/FREE would decommit, but with per-allocation mapping we'd
  // have to split frequently; instead treat as advisory no-op (V8 keeps using
  // the pages; they stay committed). Avoids mapping churn / fragmentation.
  (void)addr;
  (void)length;
  (void)advice;
  return 0;
}

int msync(void* addr, size_t length, int flags) {
  (void)addr;
  (void)length;
  (void)flags;
  return 0;
}

// Tune the JIT code-arena budget. MUST be called BEFORE V8 init (before the
// first code allocation / jitCreate); has no effect once the arena exists.
//
//   wasm_headroom_mb : extra MiB reserved for WebAssembly's separate code space.
//                      Default 64. Pass 0 for non-WASM apps to roughly HALVE the
//                      JIT arena (it is jitCreate'd and dual-mapped, so dropping
//                      64 MiB of headroom frees ~64 MiB of real memory). Useful
//                      to reclaim memory for other uses, or to fit in moderately
//                      constrained budgets.
//   max_code_mb      : hard ceiling on the code arena in MiB (never below V8's
//                      64 MiB minimum). 0 = automatic (total/3) budget guard.
//
// Example (non-WASM app): horizon_mman_set_code_budget(0, 0);
//
// NOTE: this CANNOT make full-JIT V8 coexist with a GPU (Mesa) stack in the
// tight ~137 MiB applet budget: V8's code-range floor is 64 MiB, and jitCreate
// dual-maps it to ~128 MiB real, leaving too little for Mesa's GLSL compiler.
// For a GPU canvas in applet mode, run V8 jitless instead (skips jitCreate).
void horizon_mman_set_code_budget(size_t wasm_headroom_mb, size_t max_code_mb) {
  g_wasm_headroom_mb = wasm_headroom_mb;
  g_max_code_mb = max_code_mb;
}

// Release all arena mappings + reservation. Call before the app returns to
// hbloader/hbmenu so leaked svcMapMemory aliases don't corrupt the next process.
void horizon_mman_teardown(void) {
  Lock lk;
  g_code.Teardown();
  g_arena.Teardown();
}

// --- Full-JIT write redirection support (consumed by WritableJitAllocation) ---

// If `rx_addr` is in the code arena, returns the delta to add to reach the
// writable alias (rw = rx + delta). Returns 0 otherwise (non-code memory is
// directly writable, no redirect needed).
intptr_t horizon_jit_rw_delta(uintptr_t rx_addr) {
  if (g_code.Contains(rx_addr)) return g_code.delta();
  return 0;
}

// True if `addr` is an executable code-arena (rx) address.
int horizon_jit_is_code(uintptr_t addr) { return g_code.Contains(addr) ? 1 : 0; }

// Sync I/D caches for a code range after writes via the rw alias.
void horizon_jit_sync(uintptr_t rx_addr, size_t n) { g_code.Sync(rx_addr, n); }

}  // extern "C"
