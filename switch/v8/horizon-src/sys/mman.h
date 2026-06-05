// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
//
// Minimal <sys/mman.h> shim for Nintendo Switch (Horizon / devkitA64 / libnx).
// newlib has no mmap. This provides just enough of the POSIX mmap surface that
// src/base/platform/platform-posix.cc uses.
//
// Implementation: mman-horizon.cc reserves a DATA arena from the STACK virtmem
// region and commits 16 MiB slabs lazily via svcMapMemory on first
// write-permission (see that file for the full design). Key semantics:
//  * Anonymous PROT_NONE mappings only RESERVE address space (no backing);
//    RW mappings / mprotect(RW) COMMIT the covering slabs on demand.
//  * mprotect/mmap propagate commit failure: if a slab cannot be backed they
//    return -1 / MAP_FAILED with errno=ENOMEM, so V8's PageAllocator takes its
//    graceful commit-failure path instead of faulting on a later write into
//    unbacked memory (see HEAP-COMMIT-INVESTIGATION.md).
//  * Executable (JIT) memory is served from the libnx jit_* dual-mapped code
//    arena, not here (see PORTING-NOTES.md blocker #4).
//  * madvise / msync are NO-OP successes (advisory only).
//
// The committable DATA arena is bounded by the STACK-region reservation, NOT
// the process memory grant; horizon_mman_data_arena_size() exposes the real
// ceiling so the embedder can size the V8 heap to fit.

#ifndef V8_BASE_PLATFORM_HORIZON_SYS_MMAN_H_
#define V8_BASE_PLATFORM_HORIZON_SYS_MMAN_H_

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Protection bits.
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

// Mapping flags (only the ones platform-posix.cc references).
#define MAP_FILE 0x0000
#define MAP_SHARED 0x0001
#define MAP_PRIVATE 0x0002
#define MAP_FIXED 0x0010
#define MAP_ANON 0x1000
#define MAP_ANONYMOUS MAP_ANON
#define MAP_NORESERVE 0x0040
#define MAP_LAZY 0x0000  // unsupported; treated as no-op
// MAP_JIT: serve this allocation from the libnx jit_* code arena (rx addresses,
// with a writable rw alias). Set by platform-posix.cc for executable memory.
#define MAP_JIT 0x0800

#define MAP_FAILED ((void*)-1)

// madvise advice values (all treated as no-ops).
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_DONTFORK 10
#define MADV_HUGEPAGE 14
// Apple spellings referenced by platform-posix.cc under some configs.
#define MADV_FREE_REUSABLE MADV_FREE
#define MADV_FREE_REUSE MADV_DONTNEED

void* mmap(void* addr, size_t length, int prot, int flags, int fd,
           off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t length, int prot);
int madvise(void* addr, size_t length, int advice);
int msync(void* addr, size_t length, int flags);

#ifdef __cplusplus
}
#endif

#endif  // V8_BASE_PLATFORM_HORIZON_SYS_MMAN_H_
