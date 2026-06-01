// Horizon (Nintendo Switch / newlib) support shims for Skia consumers.
//
// Skia's POSIX file port (src/ports/SkOSFile_posix.cpp) references pread(),
// which newlib does not provide. Skia compiles fine against libskia.a, but the
// symbol must be resolved at link time. switch-skia installs this object so any
// embedder linking -lskia also gets a working pread without re-implementing it.
//
// Implemented via lseek + read, restoring the file position (pread must not
// disturb the fd offset). Skia uses this only on a fallback path (when font
// file mmap is unavailable), so performance is irrelevant.

#include <unistd.h>

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved == (off_t)-1) {
    return -1;
  }
  if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
    return -1;
  }
  ssize_t n = read(fd, buf, count);
  lseek(fd, saved, SEEK_SET);  // restore original offset
  return n;
}
