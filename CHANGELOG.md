# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
