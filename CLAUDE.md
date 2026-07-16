# Requiem — cinematic convolution reverb (space)

Per-repo working memory for Claude Code sessions on this plugin. Part of the **Basilica Audio** plugin suite — sacred-architecture DSP for heavy music (`github.com/basilica-audio`).

## What this is
Requiem is the "cinematic convolution reverb (space)" member of the suite. AU / VST3 / Standalone, JUCE 8.

## Status (v0.2.0 — deep-dive DSP rework + M2 preset system done)
Core DSP working, **83 Catch2 tests green** (sample-rate sweeps 44.1-192kHz, mono/stereo bus-layout coverage, long-run NaN/Inf stability, extreme parameter automation, plus the v0.2.0 research-derived DSP guarantees and preset-system/i18n coverage). CI (macOS + Windows, pluginval strictness 10 + auval) green. GUI is a functional v0.1/v0.2 slider/combo/toggle editor plus a preset bar (custom LookAndFeel is still roadmap M3). No signing yet (roadmap M4). v0.2.0 shipped: a research-derived rebuild of the procedural IR generator (density-buildup early reflections + multiband/progressively-darkening decay, see `docs/design-brief.md`), two new parameters (Size, Bass Decay), the suite's M2 preset system (`src/presets/`, ported from `basilica-audio/nave`'s pilot — see nave's `docs/preset-system-notes.md` for the replication recipe), 11 factory presets, and a German frame-string localisation. Open work is tracked in this repo's GitHub **milestones/issues**.

## DSP
Requiem is a cinematic convolution reverb built around juce::dsp::Convolution, JUCE 8.0.14. The impulse response is generated procedurally, off the audio thread, from Decay (0.1-10s, RT60-style mid-band reference decay), Damping (500-20000 Hz — the terminal HF corner of a progressively descending high-band cutoff, not a static filter), Space (Cathedral/Hall/Chamber — shapes a density-buildup early-reflection layer), Size (0-100%, default 50% — continuously scales that early-reflection buildup/flat-window timing within Space's own envelope, decoupled from Decay/RT60), Early/Late Balance (equal-power crossfade between the early layer and the diffuse late tail), Bass Decay (25-175%, default 130% — RT60 multiplier for the low band only; the high band gets an implicit ~80% multiplier), and Freeze (flattens every band's envelope to a full-gain, non-decaying sustain, bounded to Decay seconds, forces the early layer off, and uses a static rather than time-varying high-band cutoff) — ReverbIR::generateProceduralImpulseResponse() is a pure, stateless, independently-testable function that splits the diffuse tail into low/mid/high bands (crossovers ~500Hz/~5kHz) with independent per-band decay rates, replacing v0.1's single global one-pole filter. Signal flow: input -> Pre-Delay (juce::dsp::DelayLine, 0-250ms) -> Convolution (wet) -> Modulation (juce::dsp::Chorus, wet-only, 0-100% depth, 0% = bit-identical bypass) -> Width (manual M/S scaling, wet-only, 0-200%) -> Dry/Wet Mix (juce::dsp::DryWetMixer, latency-compensated against convolution.getLatency(), which is 0 for the default zero-latency/uniform-partitioned configuration used here; Modulation adds no reported latency) -> Output trim (juce::dsp::Gain). Decay/Damping/Space/Early-Late-Balance/Freeze/Size/Bass-Decay changes only ever write to atomics from processBlock (real-time safe); actual IR regeneration happens via ReverbEngine::regenerateImpulseResponseIfNeeded(), driven by a 20 Hz juce::Timer in RequiemAudioProcessor that always runs on the message thread. Per juce::dsp::Convolution's own threading contract (JUCE 8.0.14: load() calls must be synchronised with process(), i.e. made from the audio thread), regenerateImpulseResponseIfNeeded()/loadUserImpulseResponse() only ever generate/validate off the audio thread and hand the result to a SpinLock-guarded slot; convolution.loadImpulseResponse() itself is called exclusively from ReverbEngine::process() (audio thread) — see docs/architecture.md's "Regeneration" section (preserved unchanged/unregressed from the v0.1.1 fix, per this rework's binding brief). An optional user-supplied IR file (WAV/AIFF/etc via juce::Convolution's file loader) can override the procedural generator; it is validated (readable audio, ≤30s) via a juce::AudioFormatReader before being handed to Convolution, rejecting bogus/oversized files without touching engine state; its path is persisted as a plain XML attribute alongside the APVTS state, with graceful fallback to procedural if the file has moved/been deleted on reload. Size/Bass Decay have zero effect while a user IR is active (same gate as the other procedural-only parameters).

## Presets
`src/presets/` (PresetManager/PresetBar/Localisation) implements the suite-wide M2 preset system (`.scaffold/specs/preset-system-m2.md`), ported verbatim from `basilica-audio/nave`. 11 factory presets ship under `presets/factory/*.json` (see `docs/presets.md`); user presets live at `~/Library/Audio/Presets/Yves Vogl/Requiem/` (macOS) / `%APPDATA%/Yves Vogl/Requiem/Presets/` (Windows). German frame-string localisation via `resources/i18n/de.txt`; parameter names/units are never translated.

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
