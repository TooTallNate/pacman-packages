# heap-stress

A standalone V8 app that stresses the Horizon memory paths to validate the
`switch-v8` heap/W^X fixes (see `switch/v8/HEAP-COMMIT-INVESTIGATION.md`).

It sizes the V8 heap from `horizon_mman_data_arena_size()`, then runs two
workloads and reports to the on-screen console + `sdmc:/heap-stress.log`:

- **A** — allocate 1 MiB `Uint8Array`s incrementally, touching every page, until
  it throws (caught) or hits a cap.
- **B** — a large `Array.prototype.join` (the "build an NSP in JS" shape).

A correct build prints `SURVIVED (no Data Abort)` and ends with a caught
`RangeError` at the real memory wall — it must **never** Data-Abort in the GC
(`SetOldGenerationPageFlags` / `Sweeper::ZeroOrDiscardUnusedMemory`).

Run it in both regimes:
- **applet** — launch from Album/hbmenu (~137 MiB free; runs jitless).
- **application** — hold **R** while opening a game, then hbmenu (full JIT).

Hold **+** to exit.

## Build

Requires `switch-v8` installed in portlibs (`pacman -U switch-v8-*.pkg.tar.zst`),
then:

```sh
make
```

The JIT policy is auto: full JIT in application mode, jitless when memory is
tight (applet). Flip `can_jit` in `source/main.cpp` to force a mode.
