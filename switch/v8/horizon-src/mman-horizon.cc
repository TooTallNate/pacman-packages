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
    DiagLog("mman: arena base=%p size=0x%zx\n", slice, arena_size_);
    return true;
  }

  bool Contains(uintptr_t a, size_t len) const {
    return a >= base_ && (a + len) <= base_ + arena_size_;
  }

  // Map `size` bytes at address `at` (must be free arena space). One heap block,
  // one svcMapMemory. Returns true on success.
  bool MapAt(uintptr_t at, size_t size) {
    size = RoundUpPage(size);
    void* src = memalign(kPage, size);
    if (!src) return false;
    Result rc = svcMapMemory(reinterpret_cast<void*>(at), src, size);
    if (R_FAILED(rc)) {
      if (!logged_fail_) {
        logged_fail_ = true;
        DiagLog("mman: svcMapMemory FAILED rc=0x%x at=%p size=0x%zx\n", rc,
                (void*)at, size);
      }
      free(src);
      return false;
    }
    std::memset(reinterpret_cast<void*>(at), 0, size);
    if (++map_count_ <= 3 || (map_count_ % 64) == 0) {
      DiagLog("mman: map #%u at=%p size=0x%zx (ok)\n", map_count_, (void*)at,
              size);
    }
    Region* r = static_cast<Region*>(malloc(sizeof(Region)));
    if (!r) {
      svcUnmapMemory(reinterpret_cast<void*>(at), src, size);
      free(src);
      return false;
    }
    r->dst = at;
    r->src = src;
    r->size = size;
    r->next = regions_;
    regions_ = r;
    return true;
  }

  // Bump-allocate `size` bytes of fresh, committed address space.
  void* Allocate(size_t size) {
    size = RoundUpPage(size);
    if (!EnsureInit()) return nullptr;
    uintptr_t at = RoundUpPageAddr(next_free_);
    if (at + size > base_ + arena_size_) return nullptr;
    if (!MapAt(at, size)) return nullptr;
    next_free_ = at + size;
    return reinterpret_cast<void*>(at);
  }

  // Decommit [addr, addr+size). Regions fully covered are dropped; partially
  // covered regions are unmapped and their surviving prefix/suffix re-mapped at
  // their ORIGINAL addresses.
  void Decommit(uintptr_t addr, size_t size) {
    if (base_ == 0) return;
    addr &= ~(uintptr_t{kPage} - 1);
    size = RoundUpPage(size);
    uintptr_t end = addr + size;

    Region** pp = &regions_;
    while (*pp) {
      Region* r = *pp;
      uintptr_t rs = r->dst, re = r->dst + r->size;
      if (re <= addr || rs >= end) {
        pp = &r->next;
        continue;
      }
      // Partial overlap: the surviving prefix [rs,addr) and suffix [end,re) must
      // KEEP their existing contents (e.g. V8 trims a page tail but keeps the
      // head's live objects). svcUnmapMemory un-aliases the whole region, so we
      // save survivor bytes first, then re-map fresh and restore them.
      uintptr_t pre_lo = rs, pre_hi = (addr > rs) ? addr : rs;
      uintptr_t suf_lo = (end < re) ? end : re, suf_hi = re;
      size_t pre_n = (pre_hi > pre_lo) ? (pre_hi - pre_lo) : 0;
      size_t suf_n = (suf_hi > suf_lo) ? (suf_hi - suf_lo) : 0;

      void* pre_save = nullptr;
      void* suf_save = nullptr;
      if (pre_n) {
        pre_save = malloc(pre_n);
        if (pre_save) std::memcpy(pre_save, reinterpret_cast<void*>(pre_lo), pre_n);
      }
      if (suf_n) {
        suf_save = malloc(suf_n);
        if (suf_save) std::memcpy(suf_save, reinterpret_cast<void*>(suf_lo), suf_n);
      }

      *pp = r->next;
      svcUnmapMemory(reinterpret_cast<void*>(r->dst), r->src, r->size);
      free(r->src);
      free(r);

      if (pre_n && MapAt(pre_lo, pre_n) && pre_save) {
        std::memcpy(reinterpret_cast<void*>(pre_lo), pre_save, pre_n);
      }
      if (suf_n && MapAt(suf_lo, suf_n) && suf_save) {
        std::memcpy(reinterpret_cast<void*>(suf_lo), suf_save, suf_n);
      }
      free(pre_save);
      free(suf_save);
      pp = &regions_;  // list changed
    }
  }

  void SetPerm(uintptr_t addr, size_t size, u32 perm) {
    if (base_ == 0 || !Contains(addr, size)) return;
    // BRING-UP EXPERIMENT: never downgrade below RW. svcSetMemoryPermission to
    // Perm_R/None would fault if V8 (or our code) later writes; keeping pages RW
    // sidesteps W^X faults while we get the engine running. Revisit for
    // security once it works. Perm_None guard pages also stay RW (harmless).
    (void)perm;
    svcSetMemoryPermission(reinterpret_cast<void*>(addr), RoundUpPage(size),
                           Perm_Rw);
  }

  // Unmap ALL mappings and release the address-space reservation. Required
  // before the .nro returns to hbloader/hbmenu: libnx's exit does NOT unmap
  // manual svcMapMemory regions, so leaked aliases corrupt the next homebrew's
  // address space and crash it.
  void Teardown() {
    if (base_ == 0) return;
    Region* r = regions_;
    while (r != nullptr) {
      Region* next = r->next;
      svcUnmapMemory(reinterpret_cast<void*>(r->dst), r->src, r->size);
      free(r->src);
      free(r);
      r = next;
    }
    regions_ = nullptr;
    if (reservation_ != nullptr) {
      virtmemLock();
      virtmemRemoveReservation(reservation_);
      virtmemUnlock();
      reservation_ = nullptr;
    }
    base_ = 0;
    next_free_ = 0;
    arena_size_ = 0;
    initialized_ = false;
  }

 private:
  bool initialized_ = false;
  uintptr_t base_ = 0;
  uintptr_t next_free_ = 0;
  size_t arena_size_ = 0;
  VirtmemReservation* reservation_ = nullptr;
  Region* regions_ = nullptr;
  bool logged_fail_ = false;
  unsigned map_count_ = 0;
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

  if (flags & MAP_FIXED) {
    // Decommit-in-place (PROT_NONE) or recommit a sub-range.
    if (prot == PROT_NONE) {
      g_arena.Decommit(want, length);
      return addr;
    }
    if (!g_arena.Contains(want, RoundUpPage(length))) return MAP_FAILED;
    g_arena.Decommit(want, length);     // drop whatever's there
    if (!g_arena.MapAt(want, length)) return MAP_FAILED;
    if (!(prot & PROT_WRITE)) g_arena.SetPerm(want, length, Perm_R);
    return addr;
  }

  void* result = g_arena.Allocate(length);
  if (result == nullptr) return MAP_FAILED;
  if (prot == PROT_NONE) {
    g_arena.SetPerm(reinterpret_cast<uintptr_t>(result), length, Perm_None);
  } else if (!(prot & PROT_WRITE)) {
    g_arena.SetPerm(reinterpret_cast<uintptr_t>(result), length, Perm_R);
  }
  return result;
}

int munmap(void* addr, size_t length) {
  if (addr == nullptr || addr == MAP_FAILED) return -1;
  Lock lk;
  g_arena.Decommit(reinterpret_cast<uintptr_t>(addr), length);
  return 0;
}

int mprotect(void* addr, size_t length, int prot) {
  u32 perm = (prot == PROT_NONE) ? Perm_None
             : (prot & PROT_WRITE) ? Perm_Rw
                                   : Perm_R;
  Lock lk;
  g_arena.SetPerm(reinterpret_cast<uintptr_t>(addr), length, perm);
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

// Release all arena mappings + reservation. Call before the app returns to
// hbloader/hbmenu so leaked svcMapMemory aliases don't corrupt the next process.
void horizon_mman_teardown(void) {
  Lock lk;
  g_arena.Teardown();
}

}  // extern "C"
