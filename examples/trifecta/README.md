# trifecta — V8 + Skia + libuv integration demo (Nintendo Switch)

A single homebrew app that uses all three Switch ports together, validated on
real hardware (FW 18.1.0, Atmosphère):

- **libuv** — a repeating `uv_timer` drives the frame loop; `uv_hrtime` measures
  frame time / FPS.
- **V8** — each frame runs JS `scene(t)` which computes an array of circles
  `{x, y, r, color}`; the animation logic lives in JavaScript.
- **Skia** — renders the V8-computed circles + a HUD text line (firmware shared
  font) to the screen.

Two rendering backends are provided:

| `make` target | Skia backend | Output to | Notes |
|---|---|---|---|
| `make` (default) | Ganesh **GPU** (GL/GLES via EGL + Mesa/nouveau) | EGL window surface | 60 fps |
| `make BACKEND=cpu` | **CPU** raster | libnx framebuffer blit | ~40 fps |

Copy the resulting `trifecta.nro` to `sdmc:/switch/` and launch from hbmenu.
Hold **+** to exit. Logs go to `sdmc:/trifecta.log`.

## Building

Requires `switch-v8`, `switch-skia`, `switch-libuv` (and `switch-pkg-config`)
installed:

```sh
make                 # GPU backend (default)
make BACKEND=cpu     # CPU backend
```

The Makefile pulls libuv's flags from `aarch64-none-elf-pkg-config` and links
the V8 monolith group + Skia (GL or CPU variant) + the GL/codec stack. Note that
`-lharfbuzz` (system) is linked to satisfy `switch-freetype`'s autohinter `hb_*`
references — Skia keeps its own bundled, ICU-free HarfBuzz internally.

## The memory / JIT lesson (important for embedders)

The Switch runs homebrew in two very different memory regimes, and **V8's JIT
and the GPU (Mesa) stack contend for memory in the tight one**:

| launch mode | free RAM | full-JIT V8 + GPU Skia? |
|---|---|---|
| **application** (NSP install, or hbmenu via hold-R title-redirect) | ~3 GiB | ✅ works, 60 fps |
| **applet** (NRO from Album/hbmenu) | ~137 MiB | ❌ **crashes** |

Why it crashes in applet mode: V8's full-JIT path calls libnx `jitCreate` for a
~128 MiB code region, which is **dual-mapped** (rx + rw) ≈ 254 MiB of real
memory. After the retry loop steps down to the 64 MiB floor that is still
~128 MiB committed, which on top of the ~243 MiB baseline leaves almost nothing
for **Mesa's GLSL shader compiler** — its first shader compile then NULL-derefs
deep in `_mesa_glsl_builtin_functions_init`.

(Confirmed by isolation: standalone Skia GL with no V8 renders fine in applet
mode at 137 MiB free; standalone full-JIT V8 with no GPU runs fine too. Only the
*combination* in applet mode fails.)

**The fix used by the GPU backend here: run V8 jitless in applet mode.**
`--jitless` + `code_range_size_in_bytes(0)` makes V8 skip `jitCreate` entirely,
freeing the ~128 MiB for Mesa. JS then runs in the interpreter (slower, but
fine for scene/animation logic). With this, the GPU demo hits **60 fps in
applet mode** too.

```c
// applet mode (tight RAM) + GPU Skia -> jitless so jitCreate doesn't starve Mesa
v8::V8::SetFlagsFromString("--jitless --single-threaded --single-threaded-gc "
                           "--no-concurrent-recompilation --predictable");
params.constraints.set_code_range_size_in_bytes(0);
```

### Implication for nx.js (or any V8 + GPU-canvas runtime)

nx.js ships **both** an NRO (applet) and an NSP (application), so both regimes
are real. Gate the JIT on **measured free memory**, not a hardcoded mode:

```
free = svcGetInfo(TotalMemorySize) - svcGetInfo(UsedMemorySize)
if (free comfortably covers V8's code range + the GPU/Mesa stack)
    full JIT           // NSP / full-memory: best JS performance
else
    jitless            // NRO / applet: GPU canvas fits, JS runs interpreted
```

The honest tradeoff in applet mode is **fast JS (full JIT, no GPU canvas)** vs
**GPU canvas (jitless, slower JS)** — 137 MiB can't fit both. For a graphical
runtime, GPU canvas wins, so jitless is the right applet-mode default; NSP users
get full JIT + GPU together.

## Other integration gotchas surfaced by this demo

- **libuv loop needs sockets**: `uv_default_loop()` sets up an async self-pipe
  which, on Horizon, is a loopback-TCP socket pair — so call
  `socketInitializeDefault()` before using the loop.
- **`uv_timer_start(.., repeat)`**: `repeat = 0` is a **one-shot** timer (fires
  once). Use a non-zero repeat for a frame loop.
- **Don't gate the loop on `appletMainLoop()`** in title-redirect mode — it can
  return false immediately and end your loop at frame 1. Exit on input instead.
- **EGL/GL vs the libnx console** are mutually exclusive on the one NWindow: the
  GPU backend has no console (logs to SD only); the CPU backend blits to the
  framebuffer.
- **V8 platform**: use `NewSingleThreadedDefaultPlatform()`; the multi-threaded
  default spins worker threads whose abseil sync faults on Horizon.
