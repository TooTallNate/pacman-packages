/* <sys/statfs.h> shim for Switch: struct only so libuv compiles; no statfs(). */
#ifndef NX_SYS_STATFS_H_
#define NX_SYS_STATFS_H_
#include <sys/types.h>
struct statfs {
  unsigned long f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_namelen, f_frsize, f_flags;
};
#endif
