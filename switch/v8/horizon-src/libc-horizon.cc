// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
//
// Small libc gap-fillers for Nintendo Switch (Horizon / devkitA64 / newlib).
// newlib declares these symbols' constants/prototypes but does not implement
// the functions. V8 references them, so provide minimal implementations.
//
// These are libc fallbacks, NOT part of V8's API. Other Switch ports (e.g.
// switch-libuv's libuv-horizon-port.o) defensively fill some of the same gaps
// (pthread_sigmask, sysconf). To avoid "multiple definition" link errors when
// an embedder links V8 alongside such a port, every filler here is WEAK: any
// strong definition (another port's, or the embedder's own) wins silently. The
// fillers are semantically identical trivial stubs across ports, so which one
// the linker keeps does not matter.

#include <cstdlib>
#include <cerrno>
#include <malloc.h>
#include <signal.h>
#include <unistd.h>

#include <switch.h>  // svcGetInfo, InfoType_*

#define HORIZON_WEAK __attribute__((weak))

extern "C" {

// newlib/libnx provides no pthread_sigmask (Horizon has no per-thread signal
// masks). abseil references it; provide a no-op success stub.
HORIZON_WEAK int pthread_sigmask(int /*how*/, const sigset_t* /*set*/, sigset_t* oldset) {
  if (oldset != nullptr) {
    // Report an empty old mask.
    sigemptyset(oldset);
  }
  return 0;
}

// newlib has aligned_alloc but not posix_memalign. Wrap it.
HORIZON_WEAK int posix_memalign(void** memptr, size_t alignment, size_t size) {
  // alignment must be a power of two and a multiple of sizeof(void*).
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  // aligned_alloc requires size to be a multiple of alignment.
  size_t rounded = (size + alignment - 1) & ~(alignment - 1);
  void* p = aligned_alloc(alignment, rounded);
  if (p == nullptr) return ENOMEM;
  *memptr = p;
  return 0;
}

// newlib declares _SC_* constants and sysconf() but does not implement it.
HORIZON_WEAK long sysconf(int name) {
  switch (name) {
    case _SC_PAGESIZE:  // == _SC_PAGE_SIZE
      return 0x1000;    // Horizon page size is 4 KiB.
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
      // Switch exposes up to 3 cores to applications (core 3 reserved); report
      // a conservative value. V8 tolerates this for worker-pool sizing.
      return 3;
    case _SC_PHYS_PAGES: {
      uint64_t total = 0;
      if (R_SUCCEEDED(
              svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
        return static_cast<long>(total / 0x1000);
      }
      return -1;
    }
    default:
      errno = EINVAL;
      return -1;
  }
}

}  // extern "C"
