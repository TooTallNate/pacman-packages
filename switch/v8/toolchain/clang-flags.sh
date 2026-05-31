#!/usr/bin/env bash
# Verified Clang -> aarch64-none-elf (Nintendo Switch / devkitA64) recipe.
#
# This is the toolchain recipe for building V8 (which requires Clang) against
# the devkitA64 newlib sysroot + libstdc++, while preserving Switch/Horizon ABI
# compatibility with code linked by the devkitA64 GCC driver.
#
# KEY FINDINGS (validated on this machine, see PORTING-NOTES.md "Toolchain"):
#   * Use upstream LLVM Clang (Homebrew llvm), NOT Apple Clang.
#   * devkitPro's GCC uses `-mtp=soft` because Horizon TLS must read
#     TPIDRRO_EL0 (read-only), but stock Clang has NO `-mtp=soft`; it emits a
#     raw `mrs x, TPIDR_EL0`. The Clang-equivalent is `-femulated-tls`, which
#     routes TLS through `__emutls_get_address` (provided by devkitA64 libgcc,
#     pthread-key based -> uses TPIDRRO_EL0 via libnx). DO NOT pass -mtp=soft.
#   * Include ordering matters: Clang's OWN resource-dir intrinsics
#     (arm_acle.h etc.) must come first. GCC's include dir must NOT be used for
#     intrinsics (its arm_acle.h calls __builtin_aarch64_* that Clang lacks).
#     Use -nostdinc -nostdinc++ and supply: clang-resource, libstdc++, newlib,
#     libnx -- in that order.
#   * Compile with Clang; LINK with the devkitA64 g++ driver (lld not required
#     and not present here). The GCC driver pulls switch.specs/crt/libnx.

set -euo pipefail

: "${DEVKITPRO:=/opt/devkitpro}"
: "${LLVM_BIN:=/opt/homebrew/opt/llvm/bin}"

A64="$DEVKITPRO/devkitA64"
TRIPLE="aarch64-none-elf"
CLANG="$LLVM_BIN/clang"
CLANGXX="$LLVM_BIN/clang++"

CLANG_RES="$("$CLANG" -print-resource-dir)/include"
NEWLIB="$A64/$TRIPLE/include"
# libstdc++ headers (track GCC version in the path):
GCCVER="$(basename "$(ls -d "$A64/$TRIPLE/include/c++/"*/ | head -1)")"
CXXINC="$A64/$TRIPLE/include/c++/$GCCVER"

# Switch ABI flags (match devkitPro's defaults). NOTE: -mtp=soft intentionally
# omitted; replaced by -femulated-tls for Clang.
ARCH_FLAGS=(
  --target="$TRIPLE"
  -march=armv8-a+crc+crypto
  -mtune=cortex-a57
  -femulated-tls
  -fPIE
)

COMMON_FLAGS=(
  "${ARCH_FLAGS[@]}"
  -D__SWITCH__
  # newlib hides POSIX/SVID symbols (e.g. _timezone, tzname, sem_t helpers)
  # under -std=c++NN strict-ANSI; _DEFAULT_SOURCE re-exposes them.
  -D_DEFAULT_SOURCE
  -nostdinc -nostdinc++
  -isystem "$CLANG_RES"
  -isystem "$CXXINC"
  -isystem "$CXXINC/$TRIPLE"
  -isystem "$NEWLIB"
  -isystem "$DEVKITPRO/libnx/include"
)

CXX_ONLY_FLAGS=(
  -fno-rtti
  -fno-exceptions
)

# Export as space-joined strings for use by GN/Make.
export DKP_CLANG="$CLANG"
export DKP_CLANGXX="$CLANGXX"
export DKP_CFLAGS="${COMMON_FLAGS[*]}"
export DKP_CXXFLAGS="${COMMON_FLAGS[*]} ${CXX_ONLY_FLAGS[*]}"

# Link via the GCC driver (handles crt, specs, libnx).
export DKP_LINK="$A64/bin/$TRIPLE-g++"
export DKP_LDFLAGS="-specs=$DEVKITPRO/libnx/switch.specs -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
export DKP_LDLIBS="-L$DEVKITPRO/libnx/lib -lnx"

if [ "${1:-}" = "--print" ]; then
  echo "CLANG       = $DKP_CLANG"
  echo "CLANGXX     = $DKP_CLANGXX"
  echo "CXXFLAGS    = $DKP_CXXFLAGS"
  echo "LINK        = $DKP_LINK"
  echo "LDFLAGS     = $DKP_LDFLAGS"
  echo "LDLIBS      = $DKP_LDLIBS"
fi
