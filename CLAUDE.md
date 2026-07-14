# Requiem — cinematic convolution reverb (space)

Per-repo working memory for Claude Code sessions on this plugin. Part of the **Metal up your ass** symphonic-metal plugin suite (`github.com/metal-up-your-ass`).

## What this is
Requiem is the "cinematic convolution reverb (space)" member of the suite. AU / VST3 / Standalone, JUCE 8.

## Status (v0.1 — bootstrap complete)
Core DSP working, **26 Catch2 tests green**, CI (macOS + Windows, pluginval strictness 10 + auval) green. GUI is a functional v0.1 slider editor (custom LookAndFeel is roadmap M3). No signing yet (roadmap M4). Open work is tracked in this repo's GitHub **milestones/issues**.

## DSP
Requiem is a cinematic convolution reverb built around juce::dsp::Convolution, JUCE 8.0.14. The impulse response is generated procedurally, off the audio thread, from Decay (0.1-10s, RT60-style exponential envelope) and Damping (500-20000 Hz one-pole low-pass on decorrelated stereo filtered noise) — ReverbIR::generateProceduralImpulseResponse() is a pure, stateless, independently-testable function. Signal flow: input -> Pre-Delay (juce::dsp::DelayLine, 0-250ms) -> Convolution (wet) -> Width (manual M/S scaling, wet-only, 0-200%) -> Dry/Wet Mix (juce::dsp::DryWetMixer, latency-compensated against convolution.getLatency(), which is 0 for the default zero-latency/uniform-partitioned configuration used here) -> Output trim (juce::dsp::Gain). Decay/Damping changes only ever write to std::atomic<float> from processBlock (real-time safe); actual IR regeneration + convolution.loadImpulseResponse() happens exclusively via ReverbEngine::regenerateImpulseResponseIfNeeded(), driven by a 20 Hz juce::Timer in RequiemAudioProcessor that always runs on the message thread — this was the explicit "robustness first" design constraint from the brief. An optional user-supplied IR file (WAV/AIFF/etc via juce::Convolution's file loader) can override the procedural generator; its path is persisted as a plain XML attribute alongside the APVTS state, with graceful fallback to procedural if the file has moved/been deleted on reload.

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
GitHub milestones (M1 DSP & tests · M2 presets/state · M3 GUI & a11y · M4 release/signing/v1.0.0) + issues. Read with `gh issue list --repo metal-up-your-ass/requiem`.

## Suite context
Style references: sibling `metal-up-your-ass/overture` and `metal-up-your-ass/twist-your-guts`. The suite: overture, tenebrae, nave, silentium, requiem, seraph, aureate, firmament, triptych, apotheosis, twist-your-guts.
