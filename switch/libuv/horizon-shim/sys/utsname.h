/* <sys/utsname.h> shim for Switch: uname() reports a fixed Horizon identity. */
#ifndef NX_SYS_UTSNAME_H_
#define NX_SYS_UTSNAME_H_
#include <string.h>
struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65]; };
#ifdef __cplusplus
extern "C" {
#endif
static inline int uname(struct utsname* u) {
  if (!u) return -1;
  strcpy(u->sysname,"Horizon"); strcpy(u->nodename,"switch");
  strcpy(u->release,""); strcpy(u->version,""); strcpy(u->machine,"aarch64");
  return 0;
}
#ifdef __cplusplus
}
#endif
#endif
