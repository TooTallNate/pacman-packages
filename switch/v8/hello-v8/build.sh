#!/usr/bin/env bash
# Build a hello-world .nro that links libv8_monolith.a (jitless V8 for Switch).
set -euo pipefail

DKP=/opt/devkitpro
V8=/var/folders/q4/p7rxst9x1qxgcv887_tty9yw0000gn/T/opencode/v8build/v8
OUT=$V8/out/switch
LLVM=$V8/third_party/llvm-build/Release+Asserts
TRIPLE=aarch64-none-elf
A64=$DKP/devkitA64

CLANGXX=$LLVM/bin/clang++
CRES=$LLVM/lib/clang/23/include
CXXINC=$A64/$TRIPLE/include/c++/15.2.0

HERE="$(cd "$(dirname "$0")" && pwd)"

# Compile our embedder with the SAME recipe V8 was built with.
"$CLANGXX" --target=$TRIPLE \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -femulated-tls -fPIE \
  -fno-rtti -fno-exceptions -std=c++20 -O2 -D__SWITCH__ -D_DEFAULT_SOURCE \
  -DV8_COMPRESS_POINTERS=0 -DV8_31BIT_SMIS_ON_64BIT_ARCH= \
  -nostdinc -nostdinc++ \
  -isystem "$CRES" \
  -isystem "$CXXINC" \
  -isystem "$CXXINC/$TRIPLE" \
  -isystem "$A64/$TRIPLE/include" \
  -isystem "$DKP/libnx/include" \
  -I "$V8" -I "$V8/include" \
  -c "$HERE/source/main.cc" -o "$HERE/main.o"

# Link with the devkitA64 GCC driver + libnx crt/specs, pulling in V8 + deps.
"$A64/bin/$TRIPLE-g++" \
  -specs=$DKP/libnx/switch.specs \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -Wl,-Map,"$HERE/hello-v8.map" \
  "$HERE/main.o" \
  -Wl,--start-group \
    "$OUT/obj/:v8_monolith/libv8_monolith.a" \
    "$OUT/obj/third_party/zlib:zlib/libchrome_zlib.a" \
    "$OUT/obj/third_party/zlib/google:compression_utils_portable/libcompression_utils_portable.a" \
  -Wl,--end-group \
  -L$DKP/libnx/lib -lnx \
  -o "$HERE/hello-v8.elf"

# Produce the .nro
NACP="$HERE/hello-v8.nacp"
"$DKP/tools/bin/nacptool" --create "hello-v8" "V8 port" "1.0.0" "$NACP"
"$DKP/tools/bin/elf2nro" "$HERE/hello-v8.elf" "$HERE/hello-v8.nro" --nacp="$NACP" \
  --icon="$DKP/libnx/default_icon.jpg"

echo "OK: $HERE/hello-v8.nro"
ls -lh "$HERE/hello-v8.nro"
