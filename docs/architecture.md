# Architecture

## Signal flow

```mermaid
flowchart LR
    IN[Input] --> PD[Pre-Delay<br/>0-250 ms]
    PD --> CONV[Convolution<br/>procedural/user IR]
    CONV --> MOD[Modulation<br/>chorus, wet only]
    MOD --> WIDTH[Width<br/>M/S, wet only]
    WIDTH --> MIX[Dry/Wet Mix]
    IN -.->|delay-compensated dry path| MIX
    MIX --> OUT_GAIN[Output trim]
    OUT_GAIN --> OUT[Output]

    DECAY[Decay] -.->|message thread| IRGEN[Impulse Response<br/>Generator]
    DAMPING[Damping] -.->|message thread| IRGEN
    SPACE[Space] -.->|message thread| IRGEN
    BALANCE[Early/Late Balance] -.->|message thread| IRGEN
    FREEZE[Freeze] -.->|message thread| IRGEN
    IRGEN -.->|loadImpulseResponse| CONV
    USERIR[User IR file] -.->|overrides procedural| CONV
```

Everything from Pre-Delay through Width is the "wet" path, owned by `ReverbEngine` (`src/dsp/ReverbEngine.{h,cpp}`). The dry path is the untouched input signal, delayed to stay time-aligned with the wet path's reported latency (see [Latency](#latency) below), then blended in at the Mix stage via `juce::dsp::DryWetMixer`. Output trim is applied after the mix.

## Module map

| Directory | Responsibility |
|---|---|
| `src/dsp/ImpulseResponseGenerator.{h,cpp}` | Pure, stateless procedural IR generation: decorrelated filtered-noise stereo tails with an RT60-style exponential envelope, plus a discrete early-reflection layer shaped by Space and blended in via Early/Late Balance, and a Freeze mode that flattens the envelope. No `juce::AudioProcessor`/`juce::dsp::Convolution` dependency, so it is directly unit-testable (see `tests/ImpulseResponseGeneratorTests.cpp`). Not real-time safe (allocates, does per-sample `exp()`/random calls) - called only from `ReverbEngine::prepare()` or its message-thread-only regeneration path. |
| `src/dsp/ReverbEngine.{h,cpp}` | The full signal chain: Pre-Delay, `juce::dsp::Convolution`, Modulation (`juce::dsp::Chorus`, wet only), Width (M/S), `juce::dsp::DryWetMixer`, output `juce::dsp::Gain`. Owns the split between real-time-safe parameter setters (Pre-Delay, Width, Mix, Output, Modulation - smoothed/internally-ramped, callable every block from the audio thread) and message-thread-only operations (impulse-response (re)generation/loading - now driven by Decay, Damping, Space, Early/Late Balance, and Freeze together - and user-IR load/clear, with format-reader validation and a length cap for robustness). Independent of `juce::AudioProcessor` so it is directly unit-testable (see `tests/EngineTests.cpp`). |
| `src/params` | Parameter layout and `AudioProcessorValueTreeState` definitions - parameter IDs, ranges, defaults. Single source of truth for what a preset captures (plus the non-parameter user-IR-path state key in `ParameterIds.h`'s `StateKeys` namespace). |
| `src/PluginProcessor.*` | Host plumbing: APVTS construction, `prepareToPlay`/`processBlock`/`reset`, latency reporting, state save/load (including the user-IR file path), and a `juce::Timer` that drives message-thread impulse-response regeneration. Reads APVTS values and pushes them into `ReverbEngine` every block; does not implement any DSP itself. |
| `src/PluginEditor.*` | A simple, functional v0.1 GUI: one rotary slider per float/choice parameter (Space uses a `ComboBox` via `ComboBoxAttachment`, populated directly from the parameter's own choice list; Freeze uses a `ToggleButton` via `ButtonAttachment`), plus "Load IR.../Clear IR" buttons for the user impulse-response override. A custom vector-drawn GUI is a later milestone (M3). |

Dependency direction is one-way: `PluginEditor` -> `params` (via attachments) + `PluginProcessor` (via the IR load/clear methods), and `PluginProcessor` -> `params` + `dsp`. `src/dsp` has no upward dependency on the processor or UI, which is what keeps `ReverbEngine`/`ImpulseResponseGenerator` testable in isolation.

## Procedural impulse-response generation

Requiem's reverb tail is not a physically modelled room or a captured IR library - it is generated procedurally, off the audio thread, from five parameters: Decay, Damping, Space, Early/Late Balance, and Freeze. Per channel (mono or stereo), `ReverbIR::generateProceduralImpulseResponse()` builds two layers and blends them:

**The diffuse late-tail layer** (unchanged in character from the v0.1 model):

1. Sizes the buffer to `decaySeconds * sampleRate` samples (clamped to `[0.1, 10]` s to bound memory/CPU).
2. Generates white noise from a distinct, deterministic `juce::Random` stream per channel (so stereo output is decorrelated - the source of the tail's stereo width).
3. Runs that noise through a one-pole low-pass filter whose cutoff is the Damping parameter (higher Damping Hz = brighter/less-damped tail; lower = darker).
4. Multiplies by an RT60-style exponential envelope sized so the envelope reaches -60 dBFS at `t = decaySeconds` - or, if Freeze is on, a flat (non-decaying) envelope at full gain instead (see [Freeze](#freeze) below).

**The early-reflection layer**, generated independently (its own deterministic `juce::Random` stream, so it never correlates with the diffuse noise above) and added on top:

1. Space (Cathedral/Hall/Chamber) selects a density/spread/character preset: Cathedral is long (150 ms window), dense (22 taps), slowly-decaying; Hall is the balanced default (80 ms, 14 taps); Chamber is short (35 ms), sparse (8 taps), quickly-decaying.
2. A sparse train of discrete, geometrically-decaying "taps" (with small per-tap amplitude/polarity jitter for a less mechanical pattern) is scattered across that window.
3. Tap 0 is always forced to sample 0, at fixed positive amplitude - a proxy for the earliest/loudest boundary reflection - so the generated IR's onset never depends on Space/Early-Late-Balance. This matters: Pre-Delay's timing tests measure that onset directly (see [Latency](#latency) below), and it must stay independent of these newer parameters.

**Early/Late Balance** then crossfades the two layers with an equal-power (`cos`/`sin`) curve: 0% is the early-reflection layer alone (a short, direct "slap"), 100% is the diffuse tail alone (a smooth wash, closest to the v0.1-era character), and the default (80%) keeps the tail diffuse-dominant while giving the early layer some presence.

This is a standard "filtered noise burst plus a discrete early-reflection train" algorithmic-reverb IR model - simple, cheap to generate, and good enough for a cinematic wash, at the cost of not modelling any real room's modal behaviour or a physically accurate early-reflection geometry.

### Freeze

Freeze (a boolean parameter) sustains the tail's current spectral content instead of letting it decay: when on, the diffuse layer's envelope is flat (1.0) across the whole buffer rather than RT60-decaying, **and** it plays at full gain regardless of Early/Late Balance; the early-reflection layer is suppressed entirely. In other words, a frozen tail is always the full sustained diffuse wash, never a sustained early-reflection pattern - Early/Late Balance is ignored while frozen (see `tests/ImpulseResponseGeneratorTests.cpp`'s dedicated Freeze tests for the exact contract).

The buffer length is still bounded to `decaySeconds` worth of samples - `juce::dsp::Convolution` processes a finite kernel, so this is a **bounded sustain** (as long as the Decay knob allows, up to 10 s), not a literal infinite loop. In practice this reads as "freeze the current texture" for as long as the generated kernel lasts per loop of the convolution, which is the standard behaviour of a convolution-based freeze (as opposed to a feedback-delay-network reverb, which really can sustain indefinitely).

### Modulation

A subtle post-convolution `juce::dsp::Chorus<float>` stage (see `ReverbEngine::process()`) is applied to the wet tail only, controlled by the Modulation parameter (0-100%). It exists to soften the slightly metallic/comb-filtered ring a short procedurally generated IR can have, and to add a touch of movement/richness to a static tail. Rate (0.35 Hz) and centre delay (12 ms) are fixed, non-automatable constants tuned for a subtle effect rather than an obvious chorus/flanger; only depth (mapped to `[0.05, 0.35]`) and mix (mapped to `[0, 0.5]`) scale with the Modulation parameter, both 0 at Modulation = 0% - so 0% is a bit-identical passthrough of the unmodulated tail (`tests/EngineTests.cpp`'s Modulation test verifies both the "measurably different at full depth" and "no reported-latency change" halves of this contract).

`juce::dsp::Chorus` owns its own internal `juce::dsp::DryWetMixer`, which is subject to the exact same "prime the target before `reset()`" gotcha documented below for the outer `DryWetMixer` - `ReverbEngine::prepare()` configures the chorus's rate/depth/mix/feedback from `lastModulationAmount01` *before* calling `modulationChorus.prepare()`, so the very first block after (re)prepare already reflects the last-commanded Modulation amount rather than the class's own built-in defaults (rate 1 Hz / depth 0.25 / mix 0.5).

### Regeneration is message-thread only

Generating an IR (heap allocation, per-sample `exp()`/random calls) and loading it into `juce::dsp::Convolution` are both explicitly **not** real-time safe operations, so `ReverbEngine` splits Decay/Damping/Space/Early-Late-Balance/Freeze into two paths:

- `setDecaySeconds()`/`setDampingHz()`/`setSpaceType()`/`setEarlyLateBalance()`/`setFreeze()` - real-time safe, callable every block from the audio thread. These only store the requested value in an atomic (`std::atomic<float>`, `std::atomic<int>` for the Space enum, or `std::atomic<bool>` for Freeze); no allocation, no regeneration.
- `regenerateImpulseResponseIfNeeded()` - **message-thread only**. Compares all five requested values against the last-generated ones and, only if any actually changed (a small epsilon on the float ones, to ignore floating-point noise from repeated identical automation pushes; exact comparison on Space/Freeze), regenerates the IR and calls `juce::dsp::Convolution::loadImpulseResponse()`.

`RequiemAudioProcessor` drives this via a `juce::Timer` started in `prepareToPlay()` (20 Hz - see `impulseResponseTimerHz` in `PluginProcessor.cpp`) and stopped in `releaseResources()`/the destructor. `juce::Timer` callbacks always run on the message thread, which is what makes this safe. This is the ROBUSTNESS-first v0.1 approach: no attempt is made to crossfade/interpolate between old and new IRs beyond whatever `juce::dsp::Convolution` itself does internally (per its documentation, `loadImpulseResponse()` is itself wait-free and loads the new IR on a background thread, becoming active once fully processed - so in practice a parameter change produces a clean, click-free swap once the ~20 Hz timer notices it, not a hard glitch).

### User impulse-response override

`ReverbEngine::loadUserImpulseResponse(File)` (message-thread only, e.g. from a GUI `FileChooser` callback) validates the file before touching any engine state: it opens a `juce::AudioFormatReader` for it via a dedicated `juce::AudioFormatManager` (constructed once, basic formats registered) and rejects the file - returning `false`, leaving `usingUserImpulseResponse`/the active IR completely untouched - if it isn't readable as audio (0 channels, non-positive length, or the reader itself fails to open), or if it is longer than `ReverbEngine::maxUserImpulseResponseSeconds` (30 s, generous for any real captured space while bounding the convolution engine's worst-case CPU/memory against a mis-selected non-IR file). Only once that sanity check passes does it hand the file to `juce::dsp::Convolution::loadImpulseResponse(File, ...)`, which handles format decoding/resampling internally (any format `juce::AudioFormatManager`'s basic formats support - WAV/AIFF/FLAC/etc). While active, `regenerateImpulseResponseIfNeeded()` is a no-op (Decay/Damping/Space/Early-Late-Balance/Freeze stop driving the convolution engine); `clearUserImpulseResponse()` reverts to the procedural generator immediately. The active file's path is persisted as a plain XML attribute alongside the APVTS state (see `ParameterIds.h`'s `StateKeys::userIrPath`) since it is a string, not an automatable parameter. Restoring a state whose file has moved/been deleted falls back to the procedural generator rather than failing the whole state load (see `RequiemAudioProcessor::setStateInformation` and `tests/StateTests.cpp`).

## Latency

`juce::dsp::Convolution`'s default configuration (used here - no `Latency`/`NonUniform` constructor argument) is documented as zero-latency, using a uniformly partitioned algorithm. `ReverbEngine::getLatencySamples()` queries `convolution.getLatency()` rather than assuming 0, so the plugin stays correct if a fixed-latency configuration is ever adopted instead; `RequiemAudioProcessor::prepareToPlay()` reports it via `setLatencySamples()`.

The dry path used by the Mix control is time-aligned against this (normally zero) latency the same way as the rest of the suite: `dryWetMixer.pushDrySamples()` captures the pre-processing signal before Pre-Delay/convolution/Width touch the buffer, `setWetLatency(getLatencySamples())` configures the mixer's internal delay line to match, and `mixWetSamples()` blends the two back together - so at Mix = 0% the output is a sample-accurate passthrough of the input, once shifted by `getLatencySamples()` (verified by `tests/EngineTests.cpp`'s null test, to < -90 dBFS residual).

**Pre-Delay is deliberately not part of this latency compensation.** It is an audible effect parameter - the gap between the direct sound and the reverb tail's onset - not something to hide from the listener, so it is applied only to the wet path after the dry signal has already been captured by `pushDrySamples()`. `tests/EngineTests.cpp`'s Pre-Delay tests verify this directly: feeding a unit impulse through the fully-wet engine and measuring the first non-negligible output sample shows the wet tail's onset tracks the Pre-Delay parameter (within a small tolerance for the convolution engine's own internal block/partition alignment), while Mix = 0% still nulls immediately regardless of the Pre-Delay setting. This is exactly why the early-reflection layer's tap 0 is always forced to sample 0 (see [Procedural impulse-response generation](#procedural-impulse-response-generation) above) - Space/Early-Late-Balance must never perturb that onset measurement.

**Modulation adds no reported latency either.** It is a short, continuously modulated delay line (a chorus effect), not a bulk delay - treated the same way a hardware chorus/vibrato pedal would be, not as something requiring host-side compensation. `tests/EngineTests.cpp`'s Modulation test asserts `getLatencySamples()` is identical at 0% and 100% depth.

One JUCE 8.0.14 behaviour worth calling out (shared with the rest of the suite - see e.g. Overture's `tests/DryWetMixerContractTests.cpp`): `DryWetMixer`'s internal dry/wet gain smoothers default their *target* to fully wet (`mix == 1.0`) until `setWetMixProportion()` is called, and the mixer's own `reset()` (invoked from its `prepare()`) only snaps the smoothers' *current* value to whatever *target* is set at that moment. `ReverbEngine::prepare()` works around this by calling `dryWetMixer.setWetMixProportion(lastMixProportion)` *before* its own `reset()` runs, so the mixer is already sitting at the correct dry/wet balance from the very first `process()` call. `juce::dsp::Chorus`'s own internal `DryWetMixer` is subject to the identical gotcha - see [Modulation](#modulation) above for how `ReverbEngine::prepare()` handles it the same way.

## Parameter smoothing

- **Pre-Delay** recomputes the delay line's sample count once per block from a `juce::SmoothedValue<float, ValueSmoothingTypes::Linear>` - cheap enough to do per-sample, but block-rate smoothing keeps the implementation simple and is consistent with how the rest of the suite treats coefficient-style parameters (e.g. Overture's Tight/Tone filter cutoffs).
- **Width** is likewise smoothed and resolved to a scalar once per block, then applied as a plain per-sample mid/side multiply.
- **Mix** is smoothed entirely by `juce::dsp::DryWetMixer`'s own internal ~50 ms ramp; `ReverbEngine::setMixProportion()` just forwards the target value to it every block.
- **Modulation** is smoothed by `juce::dsp::Chorus`'s own internal smoothers (a `SmoothedValue` for depth, its own `DryWetMixer`'s ~50 ms ramp for mix); `ReverbEngine::setModulationAmount()` just forwards the mapped depth/mix targets to it every block, the same pattern as Mix.
- **Output** is a plain `juce::dsp::Gain<float>` stage, which ramps sample-accurately via its own internal `SmoothedValue` (`setRampDurationSeconds`).
- **Decay**, **Damping**, **Space**, **Early/Late Balance**, and **Freeze** are not smoothed in the traditional sense at all - they only ever affect which impulse response is *generated*, on the message thread, at a bounded ~20 Hz rate (see [Regeneration is message-thread only](#regeneration-is-message-thread-only) above). There is no attempt to interpolate between the old and new IR's audio-thread `process()` behaviour beyond whatever `juce::dsp::Convolution::loadImpulseResponse()` does internally when swapping IRs mid-stream.
- All smoothers/DSP-object targets are seeded to their real starting value in `ReverbEngine::prepare()` (`lastPreDelayMs`/`lastWidthPercent`/`lastMixProportion`/`lastModulationAmount01`), so re-preparing (sample-rate change, etc.) never resets a live parameter back to a built-in default.

## Real-time safety

- `RequiemAudioProcessor::processBlock()` starts with `juce::ScopedNoDenormals`.
- All DSP state (the convolution engine, Pre-Delay delay line, Modulation chorus, dry/wet mixer, output gain) is allocated in `prepare()`/`prepareToPlay()` and never reallocated on the audio thread.
- `reset()` clears all delay-line/convolution/chorus/gain state without deallocating (`ReverbEngine::reset()`, called from both `AudioProcessor::reset()` and internally from `prepare()`).
- Parameter values are read via `apvts.getRawParameterValue()` atomics in `processBlock()`, never via `apvts.getParameter()->getValue()` (not guaranteed lock/allocation-free) and never via `String`-keyed lookups on the audio thread. This includes the `AudioParameterChoice` (Space) and `AudioParameterBool` (Freeze) parameters, both of which still expose their raw value as a plain `float` (choice index, or 0.0/1.0) via `getRawParameterValue()`.
- Decay/Damping/Space/Early-Late-Balance/Freeze changes only ever write to an atomic from `processBlock()`; the actual impulse-response (re)generation and `loadImpulseResponse()` call happen exclusively on the message thread via `RequiemAudioProcessor`'s `juce::Timer` (`timerCallback()` -> `ReverbEngine::regenerateImpulseResponseIfNeeded()`), never inside `process()`.
- `ReverbEngine::process()` treats a zero-sample block as a safe no-op before touching any filter/delay-line/convolution/chorus state.
- The Pre-Delay line's sample count is clamped to `[0, preDelayLine.getMaximumDelayInSamples()]` every block (`ReverbEngine.cpp`), which is itself sized once (250 ms at up to 192 kHz, plus margin) so `setMaximumDelayInSamples()` - which is documented as *not* real-time safe - is never called again after construction.
- The procedural IR generator's Damping cutoff is clamped below Nyquist for whatever sample rate is passed in (`ImpulseResponseGenerator.cpp`), mirroring the defensive clamping pattern used for filter cutoffs elsewhere in the suite.
- `ReverbEngine::loadUserImpulseResponse()` (and everything it calls - a `juce::AudioFormatReader`/`juce::dsp::Convolution` file load) is message-thread only, exactly like the existing procedural-regeneration path; it is never called from `process()`.

## Test coverage (M1)

Beyond the DSP-feature-specific tests referenced throughout this document, `tests/` also covers, per the M1 test-coverage milestone:

- **Sample-rate sweeps** (`tests/SampleRateSweepTests.cpp`): the null test, general finite-output/latency checks, and repeated up/down sample-rate changes, all run across 44.1/48/88.2/96/176.4/192 kHz.
- **Bus-layout configurations** (`tests/BusLayoutTests.cpp`): `isBusesLayoutSupported()` accept/reject behaviour, and actual mono/stereo processing (including switching between them across `prepareToPlay()` calls). Requiem has a single main I/O bus and no sidechain, so there is no sidechain-specific coverage to add.
- **Long-run NaN/Inf stability** (`tests/LongRunStabilityTests.cpp`): several seconds of audio across thousands of small blocks under continuous randomised automation of every parameter (including Space/Early-Late-Balance/Modulation/Freeze), plus a dedicated long-run Freeze scenario (a frozen tail processed continuously for several seconds must stay finite and sanely bounded, not just "not NaN").
- **Extreme parameter automation**: `tests/RobustnessTests.cpp`'s range-edge and rapid-automation tests, extended to cover Space/Early-Late-Balance/Modulation/Freeze at their extremes alongside the v0.1 parameters.
