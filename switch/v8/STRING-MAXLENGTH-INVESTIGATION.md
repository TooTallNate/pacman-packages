# `v8_lower_limits_mode` caps `String::kMaxLength` at 1 MiB → JS bundles >1 MiB crash

## STATUS: FIXED in switch-v8 15.0.243-6 (rebuilt + verified)

Implemented the recommended **Option 1** (decouple `String::kMaxLength` from
`V8_LOWER_LIMITS_MODE`):

- Added patch
  **`patches/0007-v8-string-maxlength-decouple-horizon.patch`** — removes the
  `#ifdef V8_LOWER_LIMITS_MODE` branch in `include/v8-primitive.h` so
  `kMaxLength` always uses the normal `(1<<29)-24` (~512 MB) value. This also
  eliminates the public-ABI define mismatch (consumers compiling without the
  define now agree with the library), and the internal
  `src/objects/string.h:kMaxLength` derives from it automatically.
- Wired into `PKGBUILD` `prepare()` (applied against `$srcdir/v8`, idempotent
  via the existing `git apply --reverse --check` guard); `pkgrel` bumped to 6.

**Rebuilt and verified on `15.0.243`:**
- Both monoliths rebuilt clean: `out/switch-jit` (full JIT, ~110 MB) and
  `out/switch` (jitless). **No `string.h` `static_assert` failures** — the
  non-lower value is self-consistent with sandbox/pointer-compression off
  (`kMaxLength*2 + kHeaderSize` ≈ 1.07 GB < `kMaxInt` 2^31-1), as predicted.
- Shipped `include/v8-primitive.h` now reports `kMaxLength = (1<<29)-24`
  unconditionally; the `1 << 20` cap is gone.
- The JSDispatchTable (16 MB) / FixedArray reductions in `globals.h` /
  `fixed-array.h` are untouched, so `Isolate::New` memory behavior is unchanged.

**Still to do (hardware):** confirm a multi-MiB JS bundle (e.g. the
`switch-nsp-forwarder` ~1.97 MiB React bundle, or a synthetic ~2–4 MiB module)
compiles + runs on device, and that `Isolate::New` still succeeds.

> Build-tree note: the working checkout under `/var/folders/.../opencode/v8build`
> had been partially reclaimed by the OS temp-dir cleaner (lost `.gn`/`DEPS`/
> `BUILD.gn`/`.git`); it was re-fetched (`fetch v8` + tag `15.0.243` +
> `gclient sync`) and all 7 patches re-applied before the rebuild above. Move
> the checkout out of the temp dir to avoid repeat GC.

## TL;DR

The `switch-v8` portlib is built with the GN arg **`v8_lower_limits_mode = true`**
(PKGBUILD line ~115, also in `gn-args.txt` / `gn-args-jit.txt`). That flag was
added for a *good and load-bearing* reason — it shrinks V8's `JSDispatchTable`
reservation from **256 MB → 16 MB**, which is what unblocked `Isolate::New` on
Horizon (see `BUILD-LOG.md` step 6).

But the *same* flag has an unrelated side effect: it lowers
**`v8::String::kMaxLength` from ~512 MB to exactly `1 << 20` (1 MiB)**. That caps
the maximum length of *any* V8 string — including the JS source string passed to
`Script::Compile` / `ScriptCompiler::CompileModule`. Consequence: **any JS
file/bundle larger than 1 MiB cannot be compiled at all.**

This was discovered when a real nx.js app (a ~1.97 MiB React bundle, the
`switch-nsp-forwarder`) crashed at startup with an Atmosphère "Undefined
Instruction" (`brk #0`) report.

## How it manifested

Atmosphère crash report (nxjs module), symbolized against a matching CI build:

```
Exception: Undefined Instruction, opcode d4200000 (brk #0)
PC -> v8::base::OS::Abort()
LR -> v8::Utils::ReportApiFailure(const char* location, const char* message)
      location = "v8::ToLocalChecked"
      message  = "Empty MaybeLocal"
stack -> run_module()  (nx.js source/main.cc)
      -> v8::MaybeLocal<v8::String>::ToLocalChecked()
```

i.e. nx.js called `String::NewFromUtf8(...).ToLocalChecked()` on the entrypoint
source, `NewFromUtf8` returned an **empty** `MaybeLocal`, and `ToLocalChecked()`
aborted the process.

### Confirmed on device (applet mode)

A diagnostic build dumped the failure state when `NewFromUtf8` returned empty:

```
run_module NewFromUtf8 EMPTY
  name=romfs:/main.js
  len=2178094                 # ~2.1 MiB source
  kMaxLength=536870888        # 512 MB  <-- value the *header* reports to nx.js
  hadException=0              # not a pending JS exception
  heap used=2775824 total=3248128 limit=96993280   # ~3 MB used of ~97 MB; NOT OOM
```

Empirical threshold on device: a **1 MiB** module loads and runs; a **2.1 MiB**
module fails. Not OOM (97 MB heap free), not a pending exception, well under the
*reported* `kMaxLength`.

## Root cause

`v8_lower_limits_mode` sets the preprocessor define `V8_LOWER_LIMITS_MODE`, and
in `include/v8-primitive.h`:

```cpp
#ifndef V8_LOWER_LIMITS_MODE
  static constexpr int kMaxLength =
      internal::kApiSystemPointerSize == 4 ? (1 << 28) - 16 : (1 << 29) - 24;
#else
  static constexpr int kMaxLength = 1 << 20;   // <-- 1 MiB
#endif
```

There is also a **define mismatch** that hid the bug: the **switch-v8 library**
is compiled *with* `V8_LOWER_LIMITS_MODE` (so the linked code enforces 1 MiB),
but **downstream consumers (nx.js `source/*.cc`) compile *without* it**, so the
header constant they see is the ~512 MB value. That is why the diagnostic above
printed `kMaxLength=536870888` while the actual enforced limit was 1 MiB. Any
consumer that trusts `String::kMaxLength` for sizing/validation will be wrong.

## Why the flag can't simply be removed

`v8_lower_limits_mode=true` is the documented "intended knob for
memory-constrained devices" and was required to get `Isolate::New` to succeed on
Horizon. In `src/common/globals.h`:

```cpp
constexpr size_t kJSDispatchTableReservationSize =
    (V8_LOWER_LIMITS_MODE_BOOL ? 16 : 256) * MB;     // <-- the memory win we need
constexpr uint32_t kJSDispatchHandleShift = V8_LOWER_LIMITS_MODE_BOOL ? 12 : 8;
```

and in `src/objects/fixed-array.h`:

```cpp
... V8_LOWER_LIMITS_MODE_BOOL ? (16 * 1024 * 1024) : (128 * 1024 * 1024);
```

So the flag bundles together at least three *independent* reductions:
1. **JSDispatchTable reservation** 256 MB → 16 MB  *(keep — load-bearing)*
2. **FixedArray max length** 128 MiB → 16 MiB        *(fine to keep)*
3. **`String::kMaxLength`** ~512 MB → 1 MiB           *(this is the problem)*

The string limit is the only one that breaks real apps; the others are fine on
Switch. The string limit and the dispatch-table reservation are **not coupled**
in V8's logic — they just happen to share the same gate macro.

## Suggested fix (for the V8 portlib agent to evaluate)

Keep `v8_lower_limits_mode=true` (for the 16 MB dispatch table) but **decouple
`String::kMaxLength`** so it is NOT lowered to 1 MiB. Options, roughly in order
of preference:

1. **Patch `include/v8-primitive.h`** so `kMaxLength` uses the normal
   (non-lower) value even when `V8_LOWER_LIMITS_MODE` is defined — e.g. make the
   `#else` branch fall through to the default formula, or pick an explicit larger
   cap (e.g. a few hundred MiB). This is a one-line public-header patch and
   automatically fixes the internal limit too, because
   `src/objects/string.h` derives it:

   ```cpp
   // src/objects/string.h:536
   static const uint32_t kMaxLength = v8::String::kMaxLength;
   ```

   The neighboring `static_assert`s (string.h ~811–824) only require
   `kMaxLength * 2 + kHeaderSize <= kSmiMaxValue` / `<= kMaxInt`, which holds
   comfortably for the non-lower 64-bit value, so raising it is self-consistent.

   **Important:** this patch must apply to the *public* header that ships in the
   portlib AND be in effect when V8 itself is compiled, so the library and all
   downstream consumers agree on the same `kMaxLength`.

2. **Fix the define mismatch regardless.** Whatever value `kMaxLength` ends up
   being, downstream consumers (nx.js) currently compile *without*
   `V8_LOWER_LIMITS_MODE`, so they see a different `kMaxLength` than the library
   enforces. Either:
   - export `V8_LOWER_LIMITS_MODE` in the package's public cflags / a pkg-config
     `Cflags:` / a bundled config header, **or**
   - (preferred, combined with option 1) make `kMaxLength` identical in both
     modes so the define no longer affects the public ABI surface that matters.

3. If a custom value is desired, define it once in a single place both V8 and
   consumers include, to avoid re-introducing a mismatch.

### Things to verify after the change
- A JS bundle of several MiB compiles + runs on device (e.g. the
  `switch-nsp-forwarder` ~1.97 MiB bundle, or a synthetic ~2–4 MiB module).
- `Isolate::New` still succeeds (i.e. the 16 MB dispatch-table reservation is
  preserved — only the string limit changed).
- No `static_assert` failures in `src/objects/string.h` at build time.
- Memory footprint at startup is unchanged (the dispatch table / FixedArray
  reductions are retained).

## nx.js side (already handled, for context)

Independent of the portlib fix, nx.js was hardening its loader: `run_module()`
and `run_script()` in `source/main.cc` no longer call `.ToLocalChecked()` on
`String::NewFromUtf8` — they check the `MaybeLocal` and fail gracefully with a
clear message instead of aborting the process. That stops the hard `brk` crash,
but apps still can't load bundles larger than `String::kMaxLength` until the
portlib raises the limit.

## Build context
- Package: `switch-v8 15.0.243-5`, V8 tag `15.0.243`.
- Relevant GN args: `v8_lower_limits_mode=true v8_enable_sandbox=false
  v8_enable_pointer_compression=false cppgc_enable_caged_heap=false
  v8_monolithic=true v8_static_library=true` (full set in `gn-args*.txt`).
- Key source references (V8 15.0.243):
  - `include/v8-primitive.h` — `String::kMaxLength` (the `#ifdef
    V8_LOWER_LIMITS_MODE` block).
  - `src/objects/string.h:536` — internal `String::kMaxLength` derived from the
    public one; `static_assert`s at ~811–824.
  - `src/common/globals.h:142-146, 606-618` — `V8_LOWER_LIMITS_MODE_BOOL`,
    `kJSDispatchTableReservationSize`, `kJSDispatchHandleShift`.
  - `src/objects/fixed-array.h:34` — FixedArray max length under lower limits.
