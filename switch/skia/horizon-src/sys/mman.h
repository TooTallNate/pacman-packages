// Minimal <sys/mman.h> shim for Skia on Nintendo Switch (Horizon / newlib).
//
// newlib has no mmap. Skia only uses mmap for OPTIONAL memory-mapping of font
// files (src/ports/SkOSFile_posix.cpp); it falls back to ordinary buffered
// reads when mmap fails. So a no-op mmap that returns MAP_FAILED is correct and
// sufficient: Skia takes the fread path. nx.js (and most embedders) feed Skia
// font bytes directly and never hit file mmap at all.
//
// This header is placed on the include path AHEAD of newlib so Skia's
// `#include <sys/mman.h>` resolves here.
#ifndef NX_SKIA_SYS_MMAN_H_
#define NX_SKIA_SYS_MMAN_H_

#include <sys/types.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void*)-1)

static inline void* mmap(void* addr, size_t length, int prot, int flags, int fd,
                         off_t offset) {
  (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
  errno = ENOMEM;
  return MAP_FAILED;  // Skia falls back to buffered reads.
}

static inline int munmap(void* addr, size_t length) {
  (void)addr; (void)length;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif  // NX_SKIA_SYS_MMAN_H_
