# Upstream and local changes

This is a modified version of **REV-246** by tehguitarist (https://github.com/tehguitarist/REV-246), licensed
AGPL-3.0 (through its use of JUCE under JUCE's AGPLv3 option). The port is licensed under the same terms (`LICENSE`,
the upstream file unchanged). It is not affiliated with or endorsed by the upstream author, nor by EMT or any other
manufacturer.

- Upstream commit: `d3587e9` (2026-11-22), version 1.0.
- `upstream/`: **unmodified** copies of that commit: `plugin/src/dsp/` (the whole reverb engine: bridge, the five
  programme cores, generated tables, transformer, ducker, stereo width), and as references for the port files
  (not compiled) `params/`, `presets/FactoryPresets.h`, `PluginProcessor.*`, `docs/ENGINE.md`, `docs/DEVELOPMENT.md`,
  `THIRD_PARTY_NOTICES.md`, `LICENSE`.
- `src/opt/`: four of those headers with speed-ups for the device (below). The build puts `src/opt` before
  `upstream/plugin/src` on the include path, so they replace upstream's; every change is marked `MPC PORT`.

## Port files (2026-11-22)

- `src/shim/juce_core/juce_core.h` (+ two one-line forwarders): the JUCE names the vendored DSP uses, without JUCE:
  `MathConstants`, `jmin`/`jmax`/`jlimit`, `exactlyEqual`, `nextPowerOfTwo`, `ignoreUnused`, `jassert` (empty, as in
  a release build), `ScopedNoDenormals` (flush-to-zero on x86, armv7 and aarch64), `LinearSmoothedValue` and
  `dsp::IIR` low/high-pass biquads, written after JUCE's own formulas.
- `src/shim/params/Parameters.h`: stands in for upstream's `params/Parameters.h`. The step tables, the joystick value
  law (`joyNumber`) and the defaults are copied from upstream's `Parameters.cpp` unchanged; the value text is written
  with `snprintf`. If a re-vendor changes a table, change it here.
- `src/engine.cpp`: upstream's `Rev246Processor` (`PluginProcessor.cpp`) as an `mpc_engine()` for the Open MPC
  wrapper, with both of upstream's routings (`mode`: 0 TRUE ST, the port's default, 1 ORIGINAL; upstream defaults to
  ORIGINAL): same signal paths, ramps and constants, at 44.1 kHz in 128-frame blocks.
  - MODE switch as upstream's (the wet dips 10 ms, the dry crossfades 20 ms), plus: engine 0's input crossfades between
    the mono sum and the left side over 10 ms, and the second engine's input fades in over 10 ms after its reset (without
    them a sine through a switch clicked 27 ms later, offline 2026-11-22); the second engine is reset and sent the
    settings only on the way to TRUE ST (upstream resets it both ways and re-sends both engines theirs).
  - Removed: the
    panel (MPC draws the skin in `vst/layout.conf`), the VU meter's LEDs, LINK and POWER (MPC has its own insert
    bypass), user preset files (MPC saves the plugin state with the project).
  - OVL: a 0 dBFS input sample (either side, after INPUT) or the A/D out of range, held 400 ms; upstream's third rule
    (its VU at +6 over 0 VU) is left out (user, 2026-11-22). A red LED on the REVERB page (`ovl`, a read-only option).
  - INPUT / OUTPUT are upstream's fader positions (0..1 along the printed scale), drawn as upstream's faders.
  - CAL (upstream: a UI setting next to the meter) is removed: the A/D's full scale (16-bit clip, storage, hiss) is
    fixed at 0 dBFS, so the converter clips where the MPC does (user, 2026-11-22).
  - Settings changes: the first applies at once, further ones within 3 blocks (8.7 ms) wait for the next free block
    (the cores' coefficient laws cost up to 3 % of a block each on a Gen1 MPC). Upstream applies every change at once.
  - XFMR off: the stage is skipped once its crossfade has ended and reset when it comes back on (upstream keeps it
    running for a warm start); the output while off is the same.
- `vst/presets.json` (written by `tools/make_presets.py` from `FactoryPresets.h`): upstream's factory list, "Init S"
  ... "Init N" then the 65 presets, with upstream's rule that a preset leaves the preferences (XFMR; here also MODE) alone.

## Speed-ups (`src/opt`, 2026-11-22)

Measured on an MPC Live (Gen1, RK3288 at 1.6 GHz, MPC OS 3.10.1) with Open MPC's `tools/bench.sh`: idle / audio
12.5 % of a core mean before, 7.6 % after; Q-Link sweep p99 44 % (with TRUE ST) before, 14.4 % after (PASS). Per
stage (`tests/micro.cpp`): the bridge 7.3 % -> 3.8 % of a block, the cores' 16-bit mode about 1 % less each.

- `dsp/engine/BridgeFilter.h`: `headF`, the head convolution in single precision on float samples, both table rows in
  one NEON pass (half the table memory). The tail's recursions stay double, as upstream requires (its two 12.1 kHz
  modes cancel by about 50 dB).
- `dsp/engine/DeviceBridge.h`: the heads read float copies of the sample rings; the 65-tap correction FIR runs in
  float NEON; the tail weights c e^(p u T) and the fractional positions u are carried from one read-out instant to the
  next by fixed per-mode factors (u moves by a fixed rational step), set exactly from the integer grid once per block,
  instead of a 1025-step table lookup plus a Taylor series (the table, about 245 kB, missed the A17's 32 kB L1 on
  every sample) and 64-bit integer divides and conversions (library calls on armv7). The recurrence is more accurate
  than upstream's series (3e-11).
- `dsp/engine/ProgramCore.h`: the 16-bit cut's default rule (toward zero) saturates, then truncates with an integer
  conversion instead of `std::trunc` (a library call on armv7 without `vrintz`): the same 16-bit word for every input.
- `dsp/ReverbEngine.h`: the program fade's gains from a table built with upstream's formula (same values), not three
  transcendental calls per device sample.
- TRUE ST, one side per engine (`setOutputs` in `ReverbEngine.h` and `DeviceBridge.h`, 2026-11-22): engine 0 feeds only
  the left output and engine 1 only the right, so each skips the other side's correction FIR, S&H lag, output read-out
  and hiss. Bit-identical to computing both (offline, every programme in FLOAT and 16-BIT). Engine 0's right side,
  switched back on for ORIGINAL, restarts from silence under the MODE dip. `src/engine.cpp` calls it only when the
  engine has it, so the reference build against `upstream/` still compiles.

**Accuracy** (`tests/render.cpp` + `tests/null_cmp.cpp`, the same engine built against `upstream/` alone and with
`src/opt`, rendered on the device): FLOAT mode -100 to -109 dB over the whole render, at most one 16-bit step at the
output. In 16-BIT mode the loops truncate every stored value, so any rounding difference eventually flips a word and
the grit's waveform decorrelates (-31 to -74 dB depending on the window), exactly as the same code would between two
compilers; its level matches the exact build's within 0.005 dB in every window of every programme.

Tried and dropped: float heads and FIR alone (about 1 %: the cost was in the table lookups), `-funsafe-math-optimizations`
(within the noise). Not tried: sharing the bridge's tail weights between TRUE ST's two engines (they follow the same
grid), since the recurrence above already brought them to about 0.02 % of a block (`tests/micro.cpp`'s "tailWeights"
and "in tail(u)" lines time the old per-sample evaluation, not the recurrence).

## Latency

Upstream reports about 1.1 ms (49 samples at 44.1 kHz) and delays the dry path to match. MPC OS has no plugin delay
compensation, so the whole insert output is 49 samples late on MPC, as it would be in a DAW without compensation.
