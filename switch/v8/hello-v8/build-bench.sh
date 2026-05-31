#!/usr/bin/env bash
# Build the V8(JIT) vs QuickJS benchmark .nro for Nintendo Switch.
set -euo pipefail

DKP=/opt/devkitpro
V8=/var/folders/q4/p7rxst9x1qxgcv887_tty9yw0000gn/T/opencode/v8build/v8
OUT=$V8/out/switch-jit
LLVM=$V8/third_party/llvm-build/Release+Asserts
TRIPLE=aarch64-none-elf
A64=$DKP/devkitA64

CLANGXX=$LLVM/bin/clang++
CRES=$LLVM/lib/clang/23/include
CXXINC=$A64/$TRIPLE/include/c++/15.2.0

HERE="$(cd "$(dirname "$0")" && pwd)"

# Compile the bench embedder (V8 + QuickJS headers).
"$CLANGXX" --target=$TRIPLE \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -femulated-tls -fPIE \
  -fno-rtti -fno-exceptions -std=c++20 -O2 -D__SWITCH__ -D_DEFAULT_SOURCE \
  -nostdinc -nostdinc++ \
  -isystem "$CRES" \
  -isystem "$CXXINC" \
  -isystem "$CXXINC/$TRIPLE" \
  -isystem "$A64/$TRIPLE/include" \
  -isystem "$DKP/libnx/include" \
  -isystem "$DKP/portlibs/switch/include" \
  -I "$V8" -I "$V8/include" \
  -c "$HERE/main-bench.cc" -o "$HERE/main-bench.o"

# Link: V8 monolith + abseil + zlib + quickjs + libnx.
"$A64/bin/$TRIPLE-g++" \
  -specs=$DKP/libnx/switch.specs \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
  -Wl,-Map,"$HERE/hello-v8-bench.map" \
  "$HERE/main-bench.o" \
  -Wl,--start-group \
    "$OUT/obj/:v8_monolith/libv8_monolith.a" \
    /tmp/libabsl_jit.a \
    "$OUT/obj/third_party/zlib:zlib/libchrome_zlib.a" \
    "$OUT/obj/third_party/zlib/google:compression_utils_portable/libcompression_utils_portable.a" \
  -Wl,--end-group \
  -L$DKP/portlibs/switch/lib -lqjs -lm \
  -L$DKP/libnx/lib -lnx \
  -o "$HERE/hello-v8-bench.elf"

# Produce the .nro
NACP="$HERE/hello-v8-bench.nacp"
"$DKP/tools/bin/nacptool" --create "hello-v8-bench" "V8 vs QuickJS" "1.0.0" "$NACP"
"$DKP/tools/bin/elf2nro" "$HERE/hello-v8-bench.elf" "$HERE/hello-v8-bench.nro" --nacp="$NACP" \
  --icon="$DKP/libnx/default_icon.jpg"

echo "OK: $HERE/hello-v8-bench.nro"
ls -lh "$HERE/hello-v8-bench.nro"
