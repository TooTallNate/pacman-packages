#!/bin/sh
# Example: link a Nintendo Switch homebrew app against switch-libuv.
#
# libuv on Horizon needs a few things beyond "-luv":
#   1. The shim header dir ahead of newlib (-isystem .../libuv-horizon-shim)
#   2. The force-included prelude (-include .../nx-prelude.h) which turns on the
#      POSIX feature macros newlib gates its pthread/sched decls behind and
#      declares the handful of functions the support object provides.
#   3. -D__SWITCH__ -D_GNU_SOURCE -DSSIZE_MAX=0x7fffffffffffffffL
#   4. The support object libuv-horizon-port.o (real pread/pwrite/readv/writev
#      plus Horizon stubs for process/signal/tty/credential libc functions).
#
# The easiest path is to consume the installed pkg-config file, which encodes
# all of the above:
#
#   source /opt/devkitpro/switchvars.sh
#   CFLAGS="$(aarch64-none-elf-pkg-config --cflags libuv)"
#   LIBS="$(aarch64-none-elf-pkg-config --libs libuv) -lnx"
#
# Or, in a standard devkitPro application Makefile, add to CFLAGS/LIBS:
#
#   CFLAGS += $(shell aarch64-none-elf-pkg-config --cflags libuv)
#   LIBS   += $(shell aarch64-none-elf-pkg-config --libs libuv) -lnx
#
# Manual invocation (single source file -> ELF -> NRO):
set -e
source /opt/devkitpro/switchvars.sh

CFLAGS="$(aarch64-none-elf-pkg-config --cflags libuv)"
LIBS="$(aarch64-none-elf-pkg-config --libs libuv)"

aarch64-none-elf-gcc \
    -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIE \
    -specs=/opt/devkitpro/libnx/switch.specs \
    $CFLAGS \
    main.c \
    -o app.elf \
    $LIBS -lnx

elf2nro app.elf app.nro
echo "built app.nro"
