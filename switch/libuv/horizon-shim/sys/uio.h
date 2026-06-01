/* <sys/uio.h> shim for Nintendo Switch: libnx defines struct iovec in
   <sys/_iovec.h> and provides readv/writev in its socket layer, but ships no
   umbrella <sys/uio.h>. Pull in the iovec struct and declare the vectored I/O
   functions libuv uses. */
#ifndef NX_SYS_UIO_H_
#define NX_SYS_UIO_H_
#include <sys/types.h>
#include <sys/_iovec.h>
#ifdef __cplusplus
extern "C" {
#endif
ssize_t readv(int, const struct iovec*, int);
ssize_t writev(int, const struct iovec*, int);
ssize_t preadv(int, const struct iovec*, int, off_t);
ssize_t pwritev(int, const struct iovec*, int, off_t);
#ifdef __cplusplus
}
#endif
#endif
