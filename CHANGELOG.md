# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.1] - 2026-07-31

Crash-fix patch release.

### Fixed

- **Unsynchronized reconfiguration racing the background IR-render thread** (PR #29). `ReverbEngine::prepare()` — invoked from the host's `prepareToPlay()` thread — mutated sample-rate/channel state and the FDN tail's delay lines with no synchronization against the background "Requiem IR Render" thread's concurrent render pass; a second instance of the same shape existed on the user-IR load/clear path (message thread vs `setStateInformation()` host thread). Reproduced deterministically (SIGSEGV, 15/15 runs) by a new cross-thread stress test; fixed by serializing all reconfiguration entry points behind a mutex the audio thread never takes. Red-verified: crash reproduces with the fix stashed, 35 clean runs with it restored. New regression guard: `tests/CrossThreadReprepareTests.cpp`.

## [0.3.0] - 2026-07-27

The "Living Tail" release. Requiem gains a second way of producing a reverb tail - a feedback delay network whose decay is fitted automatically to whatever impulse response is loaded - and the workflow controls a cinematic reverb needs in a mix.

**Everything new defaults to neutral.** `engineMode` defaults to Classic Convolution, which is the v0.2.0 engine unchanged; the wet chain's two filters and its ducker default to hard-bypass values and are not run at all at those settings. A v0.2.0 session reloaded into v0.3.0 renders bit-identically - enforced by a same-binary render-null migration test, not by inspection.

### Added

- **Engine modes.** *Classic Convolution* (default, unchanged), *Hybrid Tail* (the impulse response supplies the early field up to its measured mixing time, a sixteen-line FDN supplies the late field), and *Tail Bloom* (the full convolution with an FDN bloom layer summed underneath).
- **`FdnTail`** (`src/dsp/FdnTail.{h,cpp}`): sixteen mutually-prime, log-spaced delay lines; a Householder reflection composed with eight time-varying Givens rotations on disjoint index pairs (Schlecht & Habets, JASA 138, 2015); a ten-section attenuation cascade per line; and a structural freeze. Because both matrix factors are orthogonal at every instant, the prototype is lossless at every instant - the network cannot be destabilised by modulation, and the modulation cannot shift pitch, because no delay length ever changes.
- **`AttenuationDesigner`** (`src/dsp/AttenuationDesigner.{h,cpp}`): fits each line's graphic EQ to a target RT60(f) curve, following Schlecht & Habets (DAFx-17) and Prawda et al. (DAFx-19) - an interaction matrix with a Householder-QR pseudo-inverse precomputed per sample rate, Gauss-Newton refinement against the realised response, and the paper's ±10 dB command-gain clamp. A final stability projection then guarantees the result: the finished cascade is swept over a frequency grid and, if its peak magnitude reaches unity, the broadband gain is shifted down by exactly the excess.
- **`IrAnalysis`** (`src/dsp/IrAnalysis.{h,cpp}`): Abel-Huang normalised echo density (AES 121, 2006) for the mixing time; per-octave Schroeder backward integration (JASA 37, 1965) with an ISO 3382 -5…-35 dB regression for RT60(f); residual band energies at the mixing time; the raised-cosine splice window; and a 256-tap linear-phase correction-FIR designer following Carpentier et al. (DAFx-14, eq. 3). The procedural generator's own per-band decay is now exposed in closed form (`ReverbIR::analyticRt60Seconds`), so Hybrid mode skips the Schroeder fit for impulse responses it generated itself; user impulse responses always get the full measurement.
- **`MorphingConvolution`** (`src/dsp/MorphingConvolution.{h,cpp}`): an A/B pair of `juce::dsp::Convolution` engines with a 100 ms equal-power output crossfade on every kernel change (Wefers 2015), replacing v0.2.0's hard swap. This is what removes the audible staircase when a knob is dragged and the impulse response is regenerated twenty times a second.
- **`WetChain`** (`src/dsp/WetChain.{h,cpp}`): wet-path Low Cut and High Cut (12 dB/oct) plus an input-follower ducker, applied to the wet signal only.
- **Ten new parameters**, appended after the frozen v0.2.0 block: `engineMode`, `tailModMode`, `tailModDepth`, `tailModRate`, `bloomAmount`, `lowCut`, `highCut`, `duckAmount`, `duckAttack`, `duckRelease`.
- **Six factory presets** showcasing the new modes: Living Cathedral, Breathing Chamber, Blooming Hall, Vintage Lush Plate, Dialogue-Ducked Score, Infinite Frozen Nave. The eleven existing presets are untouched.
- **State schema versioning**: `getStateInformation()` now writes `stateSchema="3"`. Pre-v0.3.0 states carry no such attribute and are recognised by its absence; a higher schema than this build knows about loads tolerantly.
- Test suite expanded from 96 to 134 cases, with the measurable DSP claims asserted through real `process()` renders - T60(f) accuracy, stability Monte Carlo, pitch purity, freeze hold, mixing-time estimation, designer accuracy and reproducibility, splice continuity, wet-chain response and step timing, the migration render null, zero allocations, and zero latency.

### Changed

- **Freeze now branches on engine mode.** In Classic Convolution it works exactly as before (a regenerated flat-envelope kernel, bounded by Decay). In the two FDN modes it is structural: the network's attenuation is faded out to unity over 20 ms, leaving a lossless prototype that holds the circulating audio exactly and indefinitely, and the toggle takes effect within one audio block rather than waiting for a regeneration tick.
- **Impulse-response work moved off the message thread.** Generation, analysis and the FDN fit now run on a dedicated low-priority background thread; the message thread only posts parameter snapshots. The audio thread remains the only place a kernel is installed, per `juce::dsp::Convolution`'s threading contract - the v0.1.1 fix is preserved, not regressed.
- `docs/architecture.md`, `docs/manual.md` and `docs/presets.md` updated for the new modes, parameters and threading model.

### Notes

Three measurements came out looser than the implementation brief targeted. Each is documented at the assertion that pins it, with the measured numbers:

- **Modulation sidebands.** The brief asked for sidebands 40 dB below the carrier. Over the specified 0-6° rotation range that is neither attainable nor desirable - measured -27.1 dB at full depth, -36.2 dB at the 40% default, -41.9 dB at 20% - because those sidebands *are* the audible movement the feature exists to produce. The pitch-purity claim that distinguishes Matrix from Lush holds with margin: 0.57 cents at full depth against a 1 cent budget.
- **Hybrid tail echo density.** The FDN is excited by an impulse, so its echo density builds over hundreds of milliseconds, and that sparse opening always lands at the handover. Measured NED is 0.15 at 100 ms rising to 0.86 at 620 ms, against a target of 0.9 from t_mix + 50 ms. The fix is to excite the network from the early field rather than from the dry input, as Carpentier et al. do; that is a branch topology change and was left out of this release deliberately.
- **Energy decay relief at the splice.** Matching within ±1 dB per band needs the FDN's own residual energy measured by rendering the network for every design, rather than taken analytically from the curve it was just fitted to. The analytic route is what keeps a continuous Decay drag down to a re-solve, which is the headline behaviour of Hybrid mode; the cost is a per-band tilt error of a few dB rather than one.

### Third-party code

None imported. Everything is built on `juce_dsp` plus a hand-rolled FDN and solver. JUCE 8.0.14 (AGPLv3 arm) and Catch2 v3.15.2 (BSL-1.0) remain the only dependencies, both already in-tree.

## [0.2.0] - 2026-07-16

### Added

- **Research-derived deep-dive rework of the procedural impulse-response generator** (`docs/design-brief.md`/`docs/research-notes.md` - sourced from public manuals, developer interviews, trade-press reviews, and DSP/room-acoustics literature; no hardware unit or commercial plugin's actual output was measured, and no third-party impulse response was sampled or approximated):
  - **Early-reflection density-buildup model** replaces v1's single loud tap-0-then-geometric-decay shape: reflection density now builds up over a Space/Size-scaled buildup window, then holds roughly flat energy through a flat-window handoff, before the diffuse late tail takes over - matching Griesinger's documented Lexicon 224/480L energy-time-curve principle (0-50ms carries 2-3x the energy of the following 50-150ms window at default settings).
  - **Multiband decay** replaces v1's single static one-pole Damping filter applied uniformly across the whole tail: the tail is now split into low/mid/high bands (crossovers at ~500 Hz/~5 kHz), each with its own RT60-style decay rate, and the high band additionally gets a progressively descending cutoff so the tail measurably darkens as it decays (spectral centroid non-increasing over time) rather than holding one static filter color.
- **Size** parameter (0-100%, default 50%): apparent size of the space, decoupled from Decay (RT60) and Space (reflection character) - scales the early-reflection buildup/flat-window timing continuously within each Space choice's own envelope.
- **Bass Decay** parameter (25-175%, default 130%): RT60 multiplier for the low band only, relative to the mid band - bass rings measurably longer than mid/high by default, matching real-hall low-frequency-decay measurements. The high band gets an implicit (non-parameterized) ~80% RT60 multiplier.
- **M2 preset system** (`.scaffold/specs/preset-system-m2.md`), ported from `basilica-audio/nave`'s pilot implementation: `PresetManager`/`PresetBar` (factory presets embedded via BinaryData, user presets at `~/Library/Audio/Presets/Yves Vogl/Requiem/`, default resolution, dirty-state tracking, single-file and zip-bank import/export), eleven factory presets (`docs/presets.md`), and a horizontal preset-bar strip added to the top of the editor.
- **German localisation** of the preset system's frame strings (`resources/i18n/de.txt`), selected automatically from the system language; parameter names/units are never translated.
- Expanded Catch2 test suite (48 -> 83 tests): the v0.2.0 DSP guarantees (early-reflection energy ratio, density-buildup monotonicity, onset invariance, Size/Decay decoupling, multiband decay ordering, Bass Decay's monotonic low-band-only effect, progressive HF-darkening spectral-centroid monotonicity, Freeze non-periodicity, tolerant v1->v2 state import, user-IR-override unaffected by the two new parameters), plus preset-system and i18n-frame tests.

### Changed

- Damping's role narrows to setting the tail's *terminal* high-frequency corner (the descending-cutoff filter's endpoint) rather than a single static filter applied from t=0 - see the multiband-decay rework above.
- Freeze's finite-kernel design is explicitly reframed as a deliberate architectural choice (not a limitation): research into feedback-loop-based "infinite reverb" designs documents progressive HF-dulling and periodicity risk that a finite convolution kernel structurally cannot develop.
- `docs/architecture.md`, `docs/manual.md` updated for the reworked DSP, the two new parameters, and the M2 preset system; `docs/presets.md` and `docs/design-brief.md`/`docs/research-notes.md` added.

## [0.1.1] - 2026-07-16

### Changed

- Housekeeping: canonical squircle icon cutout embedded into the plugin binary (`ICON_BIG`) and README/manual, org link sweep, heavy-music copy reframe, README pointed at GitHub Releases, and the signed tag-triggered release CI workflow added.

### Fixed

- **Data race:** `juce::dsp::Convolution::loadImpulseResponse()` was called from the message thread (`ReverbEngine::regenerateImpulseResponseIfNeeded()`/`loadUserImpulseResponse()`/`clearUserImpulseResponse()`) while `process()` called `convolution.process()` concurrently on the audio thread, violating `juce::dsp::Convolution`'s documented threading contract ("load() calls must be synchronised with process() calls, which in practice means making the load() call from the audio thread" - JUCE 8.0.14). `loadImpulseResponse()` is now called *only* from `ReverbEngine::process()` (the audio thread); the message thread only ever generates the procedural buffer (or validates a candidate user IR file) and hands the request off through a `juce::SpinLock`-guarded slot for `process()` to apply, wait-free, on its next call. (#13)
- **Test coverage:** added a real dual-thread regression test (`tests/ConcurrentImpulseResponseReloadTests.cpp`) that runs `regenerateImpulseResponseIfNeeded()` and `process()` genuinely concurrently on separate `std::thread`s, exercising the reload path the rest of the suite only ever drove sequentially on one thread. (#14)
- **Test coverage:** the long-run Freeze test and the extreme-parameter-values test now pump a real message-loop dispatch (`juce::MessageManager::runDispatchLoopUntil()`) so `RequiemAudioProcessor`'s 20 Hz `juce::Timer` actually fires and drives IR regeneration for the Freeze/Space/Early-Late-Balance values they set, instead of only ever exercising the IR `prepareToPlay()` generated from the defaults. (#11)
- **DSP:** `ImpulseResponseGenerator`'s early-reflection tap placement no longer piles taps up at the last sample when the requested Decay is shorter than the active Space preset's reflection window (e.g. Decay = 0.1 s with Cathedral's 150 ms window) - the effective window is now scaled down to the buffer length. (#11)
- **Docs:** corrected `docs/architecture.md`'s claim that `juce::dsp::Chorus` needs the same manual "prime before `reset()`" workaround as the outer `DryWetMixer` - in JUCE 8.0.14, `Chorus::prepare()` calls `update()` before its own `reset()` and self-primes correctly. Softened the "Modulation at 0% is a bit-identical passthrough" wording, which is asserted but not covered by a dedicated bit-exact null test. (#11)

## [0.1.0] - 2026-07-14

### Added

- Project bootstrap: README, license, contributing guide, architecture and build docs, ADRs, and CI workflow.
- DSP core: initial working Requiem signal path with unit tests (Decay/Damping-driven procedural impulse response, Pre-Delay, Width, latency-compensated Dry/Wet Mix, Output trim, optional user impulse-response override).
- **Space** parameter (Cathedral/Hall/Chamber): shapes a discrete early-reflection tap layer ahead of the diffuse late tail, generated procedurally alongside the existing Decay/Damping-driven tail.
- **Early/Late Balance** parameter: equal-power crossfade between the early-reflection layer and the diffuse late tail baked into the generated impulse response.
- **Freeze** parameter: sustains the tail's current spectral content (flat envelope, full gain, early-reflection layer suppressed) instead of letting it decay, bounded to the Decay setting.
- **Modulation** parameter: a subtle post-convolution `juce::dsp::Chorus`-based movement applied to the wet tail only, to soften metallic ringing/add richness; 0% is a bit-identical passthrough and adds no reported latency.
- Robust user impulse-response loading: candidate files are validated (readable audio, ≤30 s) via a dedicated `juce::AudioFormatReader` check before being handed to `juce::dsp::Convolution`, rejecting unreadable or pathologically long files without altering engine state.
- Editor controls for all four new parameters (a `ComboBox` for Space, rotary sliders for Early/Late Balance and Modulation, a toggle button for Freeze), fully wired via APVTS attachments.
- Expanded Catch2 test suite (26 -> 48 tests): sample-rate sweeps (44.1-192 kHz) for the null test and general processing, mono/stereo bus-layout coverage (including `isBusesLayoutSupported()` accept/reject checks), long-run NaN/Inf stability runs under continuous full-parameter automation, and dedicated coverage for every new DSP feature (Space, Early/Late Balance, Freeze, Modulation, robust user-IR loading).
- `docs/manual.md`: a full user manual (signal flow, complete parameter reference with musical descriptions, mix-placement guidance, and usage tips).

### Changed

- Signal flow: Modulation (chorus, wet-only) now sits between Convolution and Width.
- `docs/architecture.md`, `README.md`, and `CLAUDE.md` updated to describe the expanded signal path and parameter set.
