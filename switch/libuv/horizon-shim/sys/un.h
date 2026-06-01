/* Minimal <sys/un.h> for Nintendo Switch: AF_UNIX sockaddr definition so libuv
   pipe code compiles. (Unix-domain sockets aren't supported by libnx BSD; pipe
   bind/connect will simply fail at runtime, which is acceptable.) */
#ifndef NX_SYS_UN_H_
#define NX_SYS_UN_H_
#include <sys/socket.h>
struct sockaddr_un {
  sa_family_t sun_family;
  char        sun_path[108];
};
#endif
