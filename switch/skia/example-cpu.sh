#!/usr/bin/env bash
# Example: link a Skia CPU-raster embedder against switch-skia.
#
# Skia's headers cross-reference as "include/core/SkCanvas.h" (relative to the
# Skia source root), so the package installs the tree under
# $PORTLIBS_PREFIX/include/skia and you include them WITH the include/ prefix and
# add -I $PORTLIBS_PREFIX/include/skia. (This is the opposite of V8's headers.)
#
# CPU backend: create an SkSurface over your own buffer with
# SkSurfaces::WrapPixels(SkImageInfo::Make(w,h,kBGRA_8888_SkColorType,
# kPremul_SkAlphaType), pixels, rowBytes), draw, then present the BGRA8888 buffer
# via the libnx software framebuffer (framebufferBegin/memcpy/framebufferEnd) —
# the same path nx.js uses for cairo today (BGRA8888 needs no swizzle).
#
# A devkitA64 GCC app can compile against Skia's headers and link the
# clang-built libskia.a fine (shared C++ ABI + libstdc++); clang is only needed
# to BUILD Skia, not to consume it.
set -euo pipefail
source /opt/devkitpro/switchvars.sh
DKP=/opt/devkitpro
TRIPLE=aarch64-none-elf
INC="$PORTLIBS_PREFIX/include/skia"
LIB="$PORTLIBS_PREFIX/lib"

"$DKP/devkitA64/bin/$TRIPLE-g++" \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -fno-rtti -fno-exceptions -std=gnu++20 -O2 -D__SWITCH__ \
  -I "$INC" -I "$DKP/libnx/include" \
  -c main.cc -o main.o

"$DKP/devkitA64/bin/$TRIPLE-g++" \
  -specs="$DKP/libnx/switch.specs" \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  main.o \
  -L"$LIB" \
    "$LIB/skia-horizon-port.o" -lskia -lskcms \
    -lfreetype -lharfbuzz -lbz2 -lpng -lz \
  -L"$DKP/libnx/lib" -lnx -lm \
  -o app.elf

elf2nro app.elf app.nro
echo "OK: app.nro (Skia CPU raster)"
