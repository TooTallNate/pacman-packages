/* Minimal <sys/termios.h> for Nintendo Switch (newlib): devkitA64 newlib's
   <termios.h> includes this but ships no real definitions, and Horizon has no
   TTY. libuv references termios only from tty.c. Provide just enough to compile;
   tcgetattr/tcsetattr are stubs that fail (ENOTTY), so uv_tty operations behave
   as "not a terminal" - the correct outcome on Switch. */
#ifndef NX_SYS_TERMIOS_H_
#define NX_SYS_TERMIOS_H_

#include <sys/types.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32
struct termios {
  tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
  cc_t c_cc[NCCS];
  speed_t c_ispeed, c_ospeed;
};

struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

/* c_iflag bits */
#define IGNBRK  0x00001
#define BRKINT  0x00002
#define IGNPAR  0x00004
#define PARMRK  0x00008
#define INPCK   0x00010
#define ISTRIP  0x00020
#define INLCR   0x00040
#define IGNCR   0x00080
#define ICRNL   0x00100
#define IXON    0x00400
#define IXANY   0x00800
#define IXOFF   0x01000

/* c_oflag bits */
#define OPOST   0x00001
#define ONLCR   0x00004

/* c_cflag bits */
#define CSIZE   0x00030
#define CS8     0x00030
#define PARENB  0x00100

/* c_lflag bits */
#define ISIG    0x00001
#define ICANON  0x00002
#define ECHO    0x00008
#define ECHONL  0x00040
#define IEXTEN  0x08000

/* c_cc indices */
#define VMIN    5
#define VTIME   6

/* tcsetattr actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* ioctl */
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#endif

static inline int tcgetattr(int fd, struct termios* t) { (void)fd; (void)t; errno = ENOTTY; return -1; }
static inline int tcsetattr(int fd, int act, const struct termios* t) { (void)fd; (void)act; (void)t; errno = ENOTTY; return -1; }
static inline speed_t cfgetispeed(const struct termios* t) { (void)t; return 0; }
static inline speed_t cfgetospeed(const struct termios* t) { (void)t; return 0; }
static inline int cfsetispeed(struct termios* t, speed_t s) { (void)t; (void)s; return 0; }
static inline int cfsetospeed(struct termios* t, speed_t s) { (void)t; (void)s; return 0; }

/* Standard cfmakeraw (newlib lacks it). Switch has no TTY so this is only
   exercised on stubbed termios, but keep it correct for completeness. */
static inline void cfmakeraw(struct termios* t) {
  if (!t) return;
  t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag &= ~(CSIZE | PARENB);
  t->c_cflag |= CS8;
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}

/* No pseudo-terminals on Switch. */
static inline char* ptsname(int fd) { (void)fd; errno = ENOTTY; return 0; }

#ifdef __cplusplus
}
#endif
#endif
