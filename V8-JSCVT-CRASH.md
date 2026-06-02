# switch-v8: JIT emits FJCVTZS (ARMv8.3 JSCVT) → Undefined Instruction crash on Switch

**Owner:** switch-v8 package
**Severity:** Blocker for full-JIT on hardware
**Status:** FIXED in switch-v8 15.0.243-4 (gn arg `v8_use_host_cpu_arm_features=false`).

## TL;DR of the fix

The real culprit was **not** the runtime CPU probe (that part was already correct
— `base::CPU::has_jscvt()` is `false` on Horizon, so the JIT does not emit
`fjcvtzs` at runtime). It was the **snapshot**: `mksnapshot` runs on the build
host (an Apple-Silicon Mac = ARMv8.5+), and V8's default
`v8_use_host_cpu_arm_features = true` folds the *host's* CPU features into the
cross-compiled snapshot builtins via `CpuFeaturesFromCompiler()` in
`ProbeImpl(cross_compile=true)`. That baked `FJCVTZS` (and LSE, DOTPROD, …) into
the embedded snapshot blob — instructions the A57 traps on.

Fix (no source patch needed): set **`v8_use_host_cpu_arm_features = false`** in
the V8 gn args (done in `switch/v8/PKGBUILD` `_gn_args_jit`). Then the snapshot
uses only `CpuFeaturesFromTargetOS()` (none for `V8_TARGET_OS_LINUX`), and the
runtime JIT keeps gating on `has_jscvt()` (false on Horizon). Verified: 0
`fjcvtzs` in the rebuilt embedded snapshot blob, and the `-D` define is absent
from all compile commands. This also fixes every *other* host-only feature
(LSE/DOTPROD/SHA3/PMULL1Q/…) leaking into the snapshot — a strictly more correct
fix than forcing a single `has_jscvt_ = false`.

---

### Original analysis (kept for the record — the runtime-probe hypothesis below
### was a red herring; the snapshot was the actual source)

## Symptom

A consumer (nx.js, full-JIT V8 build) crashes on real Switch hardware
(Atmosphère) as soon as sufficiently number-heavy JS runs under the optimizing
JIT. A pure-Canvas smoke test ran fine; adding a `crypto.subtle` self-test
(TypedArray / `Number`→int32 heavy) triggered the crash.

Atmosphère crash report (abridged):

```
Result:        0xA8 (2168-0000)
Exception Info:
    Type:      Undefined Instruction
    Address:   <nxjs + 0xbed208>
    Opcode:    1e7e0008
```

## Root cause

Opcode `0x1e7e0008` decodes as:

```
FJCVTZS W8, D0        ; ARMv8.3-JSCVT  (Floating-point Javascript Convert to Signed)
```

(base mask `0x1e7e0000`, `Rn=0` → D0, `Rd=8` → W8).

V8's arm64 backend emits `fjcvtzs` for JS `ToInt32`/`DoubleToInt32` fast paths
**when it believes the CPU implements the JSCVT feature.** The Nintendo Switch
SoC is a Tegra X1 with **Cortex-A57 / Cortex-A53 cores = ARMv8.0-A**, which does
**NOT** implement FJCVTZS (that's ARMv8.3-A). Executing it faults as an
Undefined Instruction.

So V8's CPU-feature detection is **falsely reporting JSCVT as available** on
Horizon. On Linux/Android, V8's `base::CPU` reads `getauxval(AT_HWCAP)` /
`HWCAP_JSCVT` (and/or parses `/proc/cpuinfo`). On Horizon there is no
HWCAP/`/proc`, so `base::CPU::has_jscvt()` is almost certainly returning a wrong
default (or the field is left uninitialized / true), and
`CpuFeatures::ProbeImpl(...)` then enables `JSCVT`.

This is **not** a consumer bug — any JIT'd, number-heavy JS will eventually hit
an `fjcvtzs` and crash. It only escaped notice because the first hardware tests
were pure Canvas drawing.

## Where to fix (V8 source)

The relevant code is in the V8 checkout (`$V8_SRC/v8`):

1. **`src/base/cpu.cc`** — `base::CPU::CPU()` constructor. On
   arm64 it sets feature bits like `has_jscvt_`. The Horizon/newlib path needs
   `has_jscvt_ = false` (and realistically all ARMv8.1+ optional features that
   A57/A53 lack: JSCVT, LSE atomics, etc. — but JSCVT is the one crashing).
   - Look for `#if V8_OS_LINUX` / `HWCAP_JSCVT` / `has_jscvt_`.
   - The existing Horizon platform porting (newlib) likely falls into an `#else`
     branch that leaves these defaulted. Force them off for Horizon.

2. **`src/codegen/arm64/cpu-features-arm64.cc`** (or
   `src/codegen/arm64/assembler-arm64.cc` `CpuFeatures::ProbeImpl`) — confirm it
   gates `JSCVT` on `cpu.has_jscvt()`. If `ProbeImpl` unconditionally enables
   JSCVT (some configs do under `enable_armv8`), that must be gated/removed for
   the Horizon target.

### Suggested minimal patch

Force JSCVT off on Horizon at the probe site. Either:

- In `base::CPU` ctor, under the Horizon/`V8_OS_HORIZON` (or the newlib `#else`)
  branch, explicitly `has_jscvt_ = false;`, **or**
- In `CpuFeatures::ProbeImpl` (arm64), wrap the
  `if (cpu.has_jscvt()) supported_ |= 1u << JSCVT;` so it is never taken on this
  target.

A robust belt-and-suspenders option: in `ProbeImpl`, for the Horizon target,
clear `JSCVT` (and any other ARMv8.1+ feature the A57 lacks) from `supported_`
regardless of detection.

> Note: there is no public `--no-...` runtime flag to disable JSCVT codegen;
> arm64 CPU features are probe-driven, not flag-gated. A build/probe patch is
> required.

### Add a new patch file

Follow the existing pattern in `switch/v8/patches/` (e.g.
`0007-v8-arm64-disable-jscvt-horizon.patch`) and wire it into the `_apply`
sequence in `switch/v8/PKGBUILD` (alongside `0003-v8-base-newlib-horizon.patch`,
which already touches `src/base`). Then bump `pkgrel`, rebuild, and reinstall.

## How to verify the fix

1. Disassemble the rebuilt libs and confirm **no** `fjcvtzs` remains reachable
   from emitted-code builtins (snapshot) — or simply rely on the runtime test
   below, since most `fjcvtzs` is JIT-generated at runtime, not in the snapshot.
2. Hardware test with nx.js (full JIT path):
   - nx.js `romfs/main.js` already contains a `crypto.subtle` self-test
     (SHA-256 KAT, getRandomValues, AES-CBC round-trip, HMAC sign/verify) plus a
     Canvas render. Before this fix it crashes with the Undefined Instruction
     above; after the fix it should render `PASS` lines and keep ticking frames.
   - Build: `export DEVKITPRO=/opt/devkitpro; make` in the nx.js repo; FTP
     `nxjs.nro` to `sdmc:/switch/`.

## Context / impact

- nx.js Phase 1 is V8 + Cairo (CPU canvas). The memory gate in
  `nx.js/source/main.cc` chooses **full JIT** when free RAM > ~300 MiB
  (the common applet case here), which is exactly when this crash fires. The
  jitless fallback (interpreter) does **not** emit `fjcvtzs`, so jitless is a
  temporary workaround but defeats the purpose of the full-JIT build.
- Decision (nx.js side): we will NOT force jitless; we want this fixed properly
  in the V8 package so full JIT works on hardware.

## Decode reference

```python
op = 0x1e7e0008
# AArch64 FJCVTZS: 0001 1110 0111 1110 0000 00 Rn Rd  (base 0x1e7e0000)
assert (op & 0xfffffc00) == 0x1e7e0000   # FJCVTZS
rn = (op >> 5) & 0x1f   # 0  -> D0
rd = op & 0x1f          # 8  -> W8
# => FJCVTZS W8, D0
```
