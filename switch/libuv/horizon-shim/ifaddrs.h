/* <ifaddrs.h> shim for Switch: no interface enumeration. getifaddrs fails so
   uv_interface_addresses() returns an error (acceptable on Switch). */
#ifndef NX_IFADDRS_H_
#define NX_IFADDRS_H_
#include <sys/socket.h>
struct ifaddrs {
  struct ifaddrs*  ifa_next;
  char*            ifa_name;
  unsigned int     ifa_flags;
  struct sockaddr* ifa_addr;
  struct sockaddr* ifa_netmask;
  union { struct sockaddr* ifu_broadaddr; struct sockaddr* ifu_dstaddr; } ifa_ifu;
  void*            ifa_data;
};
#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr
#ifdef __cplusplus
extern "C" {
#endif
int  getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);
#ifdef __cplusplus
}
#endif
#endif
