# switch-skia: Google Skia on Nintendo Switch (devkitA64 / libnx)

Skia m149 ported to the Switch, building **two static-lib variants**, both
validated rendering on real hardware (Tegra X1):

| variant | lib | backend | present path |
|---------|-----|---------|--------------|
| CPU raster | `libskia.a` + `libskcms.a` | software rasterizer | libnx framebuffer memcpy (BGRA8888) |
| GPU (Ganesh) | `libskia-gl.a` + `libskcms-gl.a` | GL/GLES via Mesa+nouveau | EGL on the default NWindow (`eglSwapBuffers`) |

Plus `skia-horizon-port.o` (a `pread` shim) and the public Skia headers under
`$PORTLIBS_PREFIX/include/skia`.

## Why this port is small (vs e.g. V8)

Skia is designed to be embedded. **No Skia source patches were needed.** The
entire Horizon delta is GN args + tiny shims:

1. **`target_os = "linux"`** — Skia has no "horizon" concept; `target_os` only
   gates POSIX *source-file lists* (SkLog_stdio, SkOSFile_posix, etc.). "linux"
   is the right POSIX bucket. The actual target arch is set by the toolchain
   (clang `--target=aarch64-none-elf`) + the devkitA64/newlib/libnx isystem
   chain, independent of `target_os`. (Skia's `gcc_like` GN toolchain is driven
   by plain `cc`/`cxx`/`ar` args — no custom toolchain file needed.)
2. **`-DSK_BUILD_FOR_UNIX`** — newlib defines none of the macros `SkFeatures.h`
   checks, so it would fall back to `SK_BUILD_FOR_MAC` and `#include
   <dispatch/dispatch.h>`. Forcing UNIX selects the POSIX `sem_t` semaphore
   backend (newlib has `<semaphore.h>`).
3. **`-DSK_FREETYPE_MINIMUM_RUNTIME_VERSION=0x020D0300`** — devkitPro ships
   FreeType 2.13.3; this (without the DLOPEN flag bit) stops
   `SkFontHost_FreeType.cpp` from `#include <dlfcn.h>` for a runtime-symbol
   fallback newlib can't provide.
4. **`horizon-src/sys/mman.h`** — a no-op `mmap` returning `MAP_FAILED`; Skia
   only uses mmap to optionally memory-map font files and falls back to buffered
   reads, so this is sufficient.
5. **`horizon-src/skia-horizon-port.c`** — `pread()` (newlib lacks it), used by
   Skia's POSIX file port. Installed as `skia-horizon-port.o` to link alongside.
6. **link `-lharfbuzz`** — devkitPro's FreeType autofitter references HarfBuzz.

Fonts reuse devkitPro's **FreeType** (`skia_use_system_freetype2`). PDF/SVG/
Skottie/DNG-RAW are disabled (not needed for a canvas runtime).

### Built-in image codecs + text shaping (so embedders don't reimplement them)

The package enables Skia's own image and text subsystems, validated on hardware,
so consumers (e.g. nx.js) can drop their manual turbojpeg/libpng/libwebp +
HarfBuzz-bridge code:

- **Image decode/encode** via `SkCodec` / `SkEncoder`: JPEG, PNG, WebP (decode),
  GIF/animation (wuffs); PNG + JPEG encode. (WebP *encode* is off — the bundled
  libwebp mux include isn't wired in this config; decode works.) These use the
  devkitPro system codec libs at link time: `-ljpeg -lpng -lwebp -lwebpdemux -lz`.
  Decode entry point: `SkImages::DeferredFromEncodedData(SkData)`.
- **Text shaping** via `SkShaper` (drives **Skia's bundled HarfBuzz** —
  `skia_use_system_harfbuzz=false` — plus `SkUnicode` over **libgrapheme**, the
  lightweight alternative to ICU, so there is NO multi-MB ICU data blob to ship).
  One call shapes UTF-8 into an `SkTextBlob`:
  `SkUnicodes::Libgrapheme::Make()` -> `SkShapers::HB::ShaperDrivenWrapper(unicode,
  fontMgr)` -> `shape()` into an `SkTextBlobBuilderRunHandler` -> `makeBlob()` ->
  `canvas->drawTextBlob()`. Shaping ships as separate libs: `libskshaper.a`,
  `libskunicode_core.a`, `libskunicode_libgrapheme.a` (and `-gl` variants).

IMPORTANT (teardown): an embedder must explicitly release Skia objects + call
`SkShaper::PurgeCaches()` / `SkGraphics::PurgeAllCaches()` before process exit;
relying on C++ static-destructor order for the shaper/unicode/harfbuzz globals
hangs/faults the exit on Horizon (observed, then fixed by explicit teardown).

## Building (non-standard source fetch)

Skia can't be a single tarball: its DEPS come via `tools/git-sync-deps` and it
uses a bundled `gn`. The PKGBUILD consumes a pre-fetched checkout:

```sh
git clone https://skia.googlesource.com/skia
( cd skia && git checkout chrome/m149 && python3 tools/git-sync-deps )
export SKIA_SRC=$PWD/skia
export SKIA_CLANG_DIR=/path/to/llvm/bin   # a clang that targets aarch64-none-elf
# (python3 + ninja on PATH)
makepkg   # builds out/horizon-cpu + out/horizon-gl, installs both variants
```

## Consuming

Skia headers cross-reference WITH the `include/` prefix
(`#include "include/core/SkCanvas.h"`), so add `-I $PORTLIBS_PREFIX/include/skia`
and include them with that prefix. See `share/switch-skia/example-cpu.sh` and
`example-gl.sh`. A stock devkitA64 GCC app links the clang-built libs fine.

- CPU: `... skia-horizon-port.o -lskia -lskcms -lfreetype -lharfbuzz -lbz2 -lpng -lz -lnx -lm`
- GPU: build with `-DSK_GL`; `... skia-horizon-port.o -lskia-gl -lskcms-gl -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lfreetype -lharfbuzz -lbz2 -lpng -lz -lnx -lm`

## Validated on hardware (FW 18.1.0, Atmosphère)

Both a CPU embedder and a Ganesh-GL embedder drew shapes (rounded rect, stroked
circle, path triangle, alpha bar) and text (Switch shared font via FreeType ->
SkTypeface -> SkFont) correctly, 1280x720. The GL backend presents directly on
the GPU swapchain (no CPU framebuffer / memcpy).
