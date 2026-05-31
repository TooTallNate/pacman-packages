#!/usr/bin/env bash
# Example: link a V8 embedder against the switch-v8 package.
#
# switch-v8 installs four static libs into $PORTLIBS_PREFIX/lib and the V8
# public headers into $PORTLIBS_PREFIX/include. An embedder must link ALL four
# inside a --start-group/--end-group (circular references), plus -lnx and -lm.
#
# IMPORTANT: this V8 is built with pointer compression DISABLED. Do NOT define
# V8_COMPRESS_POINTERS in your embedder — V8 checks that macro by *presence*, so
# any value (including 0) reads as ENABLED and aborts at v8::V8::Initialize()
# with "Embedder-vs-V8 build configuration mismatch".
set -euo pipefail

source /opt/devkitpro/switchvars.sh
DKP=/opt/devkitpro
TRIPLE=aarch64-none-elf
INC="$PORTLIBS_PREFIX/include"
LIB="$PORTLIBS_PREFIX/lib"

# Compile your embedder. Use the V8 include root so "include/v8-*.h" resolves,
# and match V8's ABI flags (no exceptions/RTTI, C++20).
"$DKP/devkitA64/bin/$TRIPLE-g++" \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -fno-rtti -fno-exceptions -std=c++20 -O2 -D__SWITCH__ -D_DEFAULT_SOURCE \
  -I "$INC" \
  -c main.cc -o main.o

# Link: V8 + abseil + zlib glue, then libnx.
"$DKP/devkitA64/bin/$TRIPLE-g++" \
  -specs="$DKP/libnx/switch.specs" \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  main.o \
  -L"$LIB" \
  -Wl,--start-group \
    -lv8_monolith -labsl -lchrome_zlib -lcompression_utils_portable \
  -Wl,--end-group \
  -L"$DKP/libnx/lib" -lnx -lm \
  -o app.elf

elf2nro app.elf app.nro
echo "OK: app.nro"
