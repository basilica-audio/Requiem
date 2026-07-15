# Requiem — cinematic convolution reverb (space)

Per-repo working memory for Claude Code sessions on this plugin. Part of the **Metal up your ass** symphonic-metal plugin suite (`github.com/basilica-audio`).

## What this is
Requiem is the "cinematic convolution reverb (space)" member of the suite. AU / VST3 / Standalone, JUCE 8.

## Status (v0.1.0 — M1 DSP completion & test coverage done)
Core DSP + M1 additions (Space, Early/Late Balance, Modulation, Freeze, robust user-IR loading) working, **48 Catch2 tests green** (sample-rate sweeps 44.1-192kHz, mono/stereo bus-layout coverage, long-run NaN/Inf stability, extreme parameter automation across every parameter). CI (macOS + Windows, pluginval strictness 10 + auval) green. GUI is a functional v0.1 slider/combo/toggle editor (custom LookAndFeel is roadmap M3). No signing yet (roadmap M4). No preset system yet (roadmap M2 — Space is a DSP-shaping parameter, not a preset manager). Open work is tracked in this repo's GitHub **milestones/issues**.

## DSP
Requiem is a cinematic convolution reverb built around juce::dsp::Convolution, JUCE 8.0.14. The impulse response is generated procedurally, off the audio thread, from Decay (0.1-10s, RT60-style exponential envelope), Damping (500-20000 Hz one-pole low-pass on decorrelated stereo filtered noise), Space (Cathedral/Hall/Chamber — shapes a discrete early-reflection tap layer), Early/Late Balance (equal-power crossfade between that early layer and the diffuse late tail), and Freeze (flattens the envelope to a full-gain, non-decaying sustain, bounded to Decay seconds, and forces the early layer off) — ReverbIR::generateProceduralImpulseResponse() is a pure, stateless, independently-testable function. Signal flow: input -> Pre-Delay (juce::dsp::DelayLine, 0-250ms) -> Convolution (wet) -> Modulation (juce::dsp::Chorus, wet-only, 0-100% depth, 0% = bit-identical bypass) -> Width (manual M/S scaling, wet-only, 0-200%) -> Dry/Wet Mix (juce::dsp::DryWetMixer, latency-compensated against convolution.getLatency(), which is 0 for the default zero-latency/uniform-partitioned configuration used here; Modulation adds no reported latency) -> Output trim (juce::dsp::Gain). Decay/Damping/Space/Early-Late-Balance/Freeze changes only ever write to atomics from processBlock (real-time safe); actual IR regeneration + convolution.loadImpulseResponse() happens exclusively via ReverbEngine::regenerateImpulseResponseIfNeeded(), driven by a 20 Hz juce::Timer in RequiemAudioProcessor that always runs on the message thread — this was the explicit "robustness first" design constraint from the brief. An optional user-supplied IR file (WAV/AIFF/etc via juce::Convolution's file loader) can override the procedural generator; it is validated (readable audio, ≤30s) via a juce::AudioFormatReader before being handed to Convolution, rejecting bogus/oversized files without touching engine state; its path is persisted as a plain XML attribute alongside the APVTS state, with graceful fallback to procedural if the file has moved/been deleted on reload.

## Build & test
```sh
export CPM_SOURCE_CACHE="$HOME/.cache/CPM"      # shared JUCE 8.0.14 + Catch2 cache
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target Tests Requiem_Standalone --parallel 4
ctest --test-dir build --output-on-failure
```
Release/universal + pluginval + auval run in CI, not locally.

## Conventions & guardrails
- JUCE 8.0.14 via CPM · C++20 · AGPLv3 · Pamplejuce `SharedCode` pattern · manufacturer `Yvsv`, plugin code `Rqem`, `com.yvesvogl.requiem`.
- **Real-time safety:** no alloc/lock/file-IO/logging on the audio thread; allocate in `prepareToPlay`; `reset()` clears all state; `ScopedNoDenormals`; smoothed params; report latency via `setLatencySamples` where the chain adds any.
- **DryWetMixer gotcha (JUCE 8.0.14):** prime `setWetMixProportion(mix)` before `reset()` in `prepare()` (else it ramps from 100% wet). See sibling `overture`.
- **`main` is protected** — no direct commits; feature branch + PR, green CI required (Conventional Commits). New DSP needs tests (null/reference, NaN/Inf sweep, state round-trip, latency).

## Roadmap
GitHub milestones (M1 DSP & tests · M2 presets/state · M3 GUI & a11y · M4 release/signing/v1.0.0) + issues. Read with `gh issue list --repo basilica-audio/requiem`.

## Suite context
Style references: sibling `basilica-audio/overture` and `basilica-audio/crypta`. The suite: overture, tenebrae, nave, silentium, requiem, seraph, aureate, firmament, triptych, apotheosis, crypta.
