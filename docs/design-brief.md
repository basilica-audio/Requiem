# Requiem — Design Brief v2 (binding; supersedes v1 entirely)

The cinematic/orchestral convolution reverb, rebuilt against the documented behavior of the
category-defining units — the Bricasti M7's three-engine split, the Lexicon 224/480L
early-reflection design principles (David Griesinger), Altiverb's scoring-stage-derived
Size/Early/Tail axes, and general room-acoustics multiband-decay literature — packaged as a
procedural-IR engine that never licenses or bundles a sample library.
Research-driven rewrite: every default below is sourced (see docs/research-notes.md).
**No brand or person names in parameters, UI or marketing copy** — v1's naming was already
generic ("Decay", "Damping", "Space", "Early/Late Balance") and stays that way; the manual
may cite public interviews/manuals as sources.

## Why v1 falls short (the two core corrections)

1. **Early reflections decay from a single loud first tap; real ones build up density.**
   v1's `addEarlyReflections()` places a full-gain tap at sample 0 and lets every subsequent
   tap fall off geometrically (`tapDecayFactor` 0.85/0.75/0.65) across a fixed window. Every
   researched reference — Griesinger's documented energy-time profile for the Lexicon
   224/480L, and Bricasti's dedicated dense early-reflection engine — instead builds
   **density up** over the first tens of milliseconds, holds energy roughly flat through
   ~160ms (the perceptually "worst" window for direct-sound localization, which real halls
   don't try to make quiet), and only then hands off to exponential late decay. v1's shape
   is close to the opposite of this.
2. **Damping is one global low-pass from t=0; real tails darken progressively and let bass
   ring longer than highs.** v1 applies a single one-pole filter to the noise source before
   any envelope is applied, so the *spectral balance* of the tail never changes over its
   life — only its overall level does. Room-acoustics measurement and every multiband-decay
   reverb design researched (LiquidSonics Reverberate's per-band decay multipliers; real
   RT60-vs-frequency data showing bass RT60 routinely 2-3x longer than treble RT60 in the
   same hall) show the tail should get *progressively darker as it decays*, with low
   frequencies decaying more slowly than mid/high — not a flat, static filter color.

## Topology (largely unchanged; procedural IR generator is where v2 lives)

```
input -> Pre-Delay -> Convolution (procedural or user IR) -> Modulation (chorus, wet only)
      -> Width (M/S, wet only) -> Dry/Wet Mix (latency-compensated) -> Output -> output
```

Signal-path topology is validated by research (Section 5/predelay range, Section 4/freeze
in research notes) and is **not** changing structurally. What changes is entirely inside
`ReverbIR::generateProceduralImpulseResponse()`: the early-reflection generator and the
damping/decay model. Decay, Damping, Space, Early/Late Balance, Size (new), Bass Decay
(new), and Freeze still only ever drive the background message-thread IR regeneration —
the real-time-safety architecture in `docs/architecture.md` is unchanged and stays binding.

## Module specifications (authentic behaviors, generically named)

### Impulse-response generator — early-reflection buildup model (replaces v1's geometric-decay taps)

- Replace the flat geometric-decay tap train with a **density-buildup + flat-energy-window**
  model, matching the documented Lexicon 224/480L design principle: tap density increases
  across the first `earlyBuildupMs` (research-derived default **35 ms**, scaled by the new
  `Size` parameter), then **holds roughly flat total energy** through `earlyFlatEndMs`
  (research-derived default **160 ms**, per Griesinger's "worst window for intelligibility,
  don't make it decay either" finding), then hands off into the late diffuse tail rather
  than crossfading against it at one fixed balance point.
- Tap 0 stays forced to sample 0 (v1's onset-anchoring behavior is correct and matches
  Altiverb's Direct/Early split — keep it).
- `earlyBuildupMs`/`earlyFlatEndMs` are **not** exposed as separate user parameters in v2
  (M2 scope discipline) — they are internal constants driven by the existing `Space` choice
  and the new continuous `Size` parameter (see below), consistent with how Bricasti/Altiverb
  separate a small number of user-facing knobs from a denser internal early-reflection
  engine.
- Total early-reflection energy in the 0-50ms window should measure **2-3x** the energy in
  the following 50-150ms window at default settings (Griesinger's documented ratio) — this
  is a testable target, not a UI parameter.

### Size (new parameter, continuous, decoupled from Decay)

- `size` 0-100%, default **50%**. Independent axis from `Decay` (RT60/tail length) and
  `Space` (reflection character/density profile) — mirrors Bricasti's and Altiverb's
  explicit separation of "Size" (apparent dimensions / early-reflection spacing) from
  "Decay Time" (RT60) and from the discrete space-character choice. Scales
  `earlyWindowMs`/`earlyBuildupMs` continuously within each `Space` choice's envelope
  (e.g. Hall at Size 0% behaves close to today's v1 Chamber early-reflection spacing; Hall
  at Size 100% approaches today's v1 Cathedral spacing) rather than Space alone doing all
  the work. Research-derived (no source gives an exact 0-100% curve for this — reasoned
  from Bricasti/Altiverb's documented existence of a decoupled Size control).

### Damping → Bass Decay (Damping's role narrows to HF character; new second axis for the spectral tilt over time)

- `damping` keeps its existing 500-20000 Hz range/8000 Hz default/log taper (sourced range
  already reasonable; no change) but changes role: it now sets the **terminal HF corner** of
  a decay that **progressively darkens** rather than a single static filter applied
  uniformly across the whole buffer. Implementation: split the noise-based tail generation
  into low/mid/high bands (crossovers research-derived at **~500 Hz** and **~5 kHz**,
  matching LiquidSonics Reverberate's documented ~600 Hz/~6 kHz precedent) and apply the
  existing one-pole coefficient to the high band with an **additional time-varying
  tightening** so the effective HF cutoff descends over the length of the tail (modest,
  monotonic — not a second LFO), producing the "starts bright, darkens as it decays" shape
  documented for real halls (air absorption + surface absorption compounding over the
  reflection path length).
- New `bassDecay` parameter: **25-175%** multiplier on RT60 for the low band only (below the
  ~500 Hz crossover), default **130%** (bass rings measurably longer than the mid/high
  bands by default, matching the documented "RT60 = 2.0s at 125Hz vs 0.8s at 1kHz" real-hall
  ratio — 130% is a reasoned middle value, not a hardware-measured one). Range/style
  directly modeled on LiquidSonics Reverberate's documented 25-175% multiplier convention.
- High band gets an **implicit** (non-parameterized) decay multiplier of ~80% (highs finish
  measurably before the mid band) — research-derived from the same real-hall HF-absorption
  finding; not exposed as a separate knob to keep the parameter count sane for M2.

### Space (unchanged choices, retuned early-reflection profile)

- Cathedral / Hall / Chamber selector stays (default Hall, index 1) — now feeds the
  density-buildup model above (window/build-up timing) instead of the flat-decay tap train.
  Cathedral's early window widens further with Freeze-adjacent long-Decay use cases in mind
  (choir bloom); Chamber's stays tight. Exact per-Space numbers remain reasoned (no source
  publishes exact millisecond tables for a generic "Cathedral/Hall/Chamber" 3-way) but the
  *shape* (buildup + flat window, not geometric decay) is now sourced.

### Early/Late Balance (unchanged range; crossfade replaced by build-up handoff)

- Range/default (0-100%, default 80%) unchanged — still crossfades early character against
  diffuse tail dominance. Internally, the crossfade point now respects the ~160ms handoff
  window from the buildup model above rather than blending two independently-shaped layers
  at an arbitrary equal-power ratio irrespective of the buildup timing.

### Freeze (unchanged behavior — explicitly preserved, not "fixed")

- Freeze stays a finite, statically-generated convolution kernel bounded to `Decay` seconds
  with a flat envelope, exactly as v1. Research confirms this is the *right* choice, not a
  gap: feedback-loop-based infinite reverbs are documented to progressively dull (repeated
  filtering in the feedback path) or develop periodicity artifacts as feedback/diffusion
  order increases — Requiem's convolution-based freeze structurally cannot do either. v2
  adds no new Freeze parameters; it gets a one-line manual/honesty callout instead (see
  below) so this is understood as a deliberate architectural choice, not an oversight.

### Pre-Delay, Width, Mix, Output, Modulation (unchanged)

- All ranges/defaults already match the researched convention (10-30ms predelay window
  documented across mixing references; Requiem's 0-250ms range with 20ms default already
  sits inside it) or are outside this deep-dive's category-defining-behavior scope (Width,
  Mix, Output, Modulation are generic mix-utility controls, not reverb-character controls
  the reference units define distinctively). No parameter or default changes.

## Factory Presets (M2 preset system — proposed, not yet implemented)

1. **Cathedral Wash** — Space=Cathedral, Size=85%, Decay=6.5s, Damping=6500Hz,
   BassDecay=150%, Early/Late=90%, Mix=40%. Long, dark, enveloping — the default "big
   choir/strings" starting point.
2. **Concert Hall** — Space=Hall, Size=50%, Decay=2.2s, Damping=9000Hz, BassDecay=125%,
   Early/Late=80%, Mix=30%. The general-purpose orchestral-bus default (close to current
   v1 factory defaults, now spectrally correct).
3. **Chamber Room** — Space=Chamber, Size=25%, Decay=1.0s, Damping=11000Hz, BassDecay=110%,
   Early/Late=70%, Mix=25%. Intimate, small-ensemble character.
4. **Choir Bloom** — Space=Cathedral, Size=90%, Decay=5.0s, Damping=5500Hz (dark, tames
   sibilance per manual tip), BassDecay=140%, Early/Late=95%, PreDelay=35ms, Mix=45%.
5. **Tight Rhythmic Hall** — Space=Hall, Size=40%, Decay=1.6s, Damping=8500Hz,
   PreDelay=90ms (fast material stays intelligible per predelay research), Early/Late=60%,
   Mix=25%.
6. **Frozen Drone** — Space=Chamber (early layer forced off by Freeze anyway), Decay=8s,
   Damping=4000Hz, BassDecay=175%, Freeze=on, Modulation=20%, Width=140%, Mix=60%. Ambient
   pad/transition-hold starting point.
7. **Dark Sanctuary** — Space=Cathedral, Size=100%, Decay=9s, Damping=3500Hz,
   BassDecay=175%, Early/Late=100% (pure diffuse wash, no distinct early character),
   Mix=35%.
8. **Bright Slap Chamber** — Space=Chamber, Size=15%, Decay=0.6s, Damping=16000Hz,
   BassDecay=100%, Early/Late=20% (distinct, audible early reflections — the "small room
   liveness" v1's manual describes), Mix=20%.
9. **Full Wet Send Hall** — Space=Hall, Size=55%, Decay=2.5s (v1's global default decay,
   kept as a nod to continuity), Damping=8000Hz, BassDecay=130% (new global default),
   Mix=100% (dedicated aux/return-bus use case documented in v1's manual).
10. **Subtle Air** — Space=Hall, Size=30%, Decay=1.2s, Damping=12000Hz, BassDecay=115%,
    Modulation=15% (v1 manual's "sounds static/synthetic" fix), Mix=12%. Barely-there
    ambience for sources that just need a hint of space.

Preset system implementation (state schema, factory-vs-user storage, recall UI) is M2 scope
and out of this brief's boundary — the above are intent + rough settings for whoever
implements M2 to encode.

## Guarantees & tests (Catch2; keep all 48 still-valid v1 cases, adapt the rest; ≥60 total)

1. **Early-reflection energy ratio:** at default settings (Early/Late Balance=80%, any
   Space), sum |sample|² in the 0-50ms window of the generated IR is 2-3x the sum in the
   following 50-150ms window (Griesinger's documented ratio) — direct proof the buildup
   model replaced the geometric-decay shape.
2. **Early-reflection density buildup:** measured tap/peak density (local maxima count) in
   successive 10ms sub-windows across `earlyBuildupMs` is non-decreasing — proves density
   *builds up* rather than decaying from tap 0, the core v1→v2 correction.
3. **Onset invariance (kept from v1):** tap 0 stays fixed at sample 0 regardless of Space/
   Size/Balance — Pre-Delay timing still measures a stable onset.
4. **Size decoupling:** sweeping `Size` 0→100% at fixed `Decay` changes the measured RT60 by
   less than a defined small tolerance (Size must not leak into decay time) while
   measurably changing the early-reflection window/density — proves Size and Decay are
   actually independent axes.
5. **Multiband decay ordering:** band-pass the generated IR into low (<500Hz)/mid/high
   (>5kHz) and measure per-band RT60 (time to −60dB via linear regression on the log
   envelope, matching room-acoustics RT60 measurement convention); assert
   lowRT60 > midRT60 > highRT60 at default `bassDecay` (130%), and assert sweeping
   `bassDecay` from 25%→175% moves the measured low-band RT60 monotonically while leaving
   mid/high RT60 materially unchanged.
6. **Progressive HF darkening:** spectral centroid of the tail measured in successive time
   windows is non-increasing across the buffer at default settings — proves the tail gets
   darker as it decays rather than holding one static filter color throughout (the v1
   defect being corrected).
7. **Freeze envelope invariant (kept from v1, re-asserted):** frozen IR envelope stays flat
   (1.0) across its full length, bounded to `Decay` seconds — explicitly proves the finite-
   kernel design is unchanged/intentional, not a regression.
8. **Freeze non-periodicity:** FFT of a frozen IR shows no comb-filter spikes correlated
   with any short internal repeat length (there is none by construction) — a regression
   guard proving Freeze was never reimplemented as a short looped buffer.
9. **Existing v1 guarantees, retained/adapted:** RT60 measurement at the *overall* (mid-band
   reference) level still lands within tolerance of the `Decay` parameter across its full
   0.1-10s range and 44.1-192kHz sample rates; Damping's log-taper frequency range still
   clamps below Nyquist at low sample rates; NaN/Inf sweep across every parameter incl. the
   two new ones; state round-trip incl. tolerant v1→v2 import (unknown/removed v1 param IDs
   ignored, `size`/`bassDecay` default to their v2 defaults when absent from a loaded v1
   state); latency stays 0 (convolution's zero-latency uniform-partition config unchanged);
   bus-layout coverage (mono/stereo) unchanged; long-run NaN/Inf stability with the new
   multiband generator under extreme automation of Size/BassDecay.
10. **User IR override unaffected:** loading a user IR still bypasses the entire procedural
    generator (early-buildup model, multiband decay, Size, BassDecay) exactly as v1's
    `usingUserImpulseResponse` gate already guarantees — regression test asserting the new
    parameters have zero effect on output while a user IR is active.

## Honesty & framing

- `docs/research-notes.md` ships the sourced findings (quotes + URLs) — the voicing is
  **research-derived from public manuals, developer interviews, trade-press reviews, and
  DSP/room-acoustics literature**, not measured against Bricasti/Lexicon/Altiverb hardware
  or software output, and no impulse response from any commercial IR library was sampled,
  copied, or approximated. Say so in the manual.
- Numbers fall into two explicit classes and the manual/comments should keep saying which:
  (a) **directly sourced** (predelay 10-30ms convention; Griesinger's 2-3x early-energy
  ratio and ~160ms handoff window; LiquidSonics' 25-175% band-decay-multiplier convention
  and ~600Hz/~6kHz crossover precedent; real-hall bass-vs-treble RT60 ratio), and
  (b) **research-derived/reasoned** (exact per-Space millisecond tables; the 130%/80% default
  bass/high multipliers; the Size 0-100% curve) where a source establishes the *principle*
  but not an exact number for Requiem's specific parameter space.
- Manual notes that Freeze's finite-kernel design is a deliberate divergence from
  feedback-loop-based "infinite reverb" plugins, with the documented reasoning (no
  progressive dulling, no periodicity risk) rather than presenting it as a limitation.
- Out of scope for v2 (explicitly): exposing `earlyBuildupMs`/`earlyFlatEndMs` as user
  parameters, a fourth Space option, sidechain-ducked reverb, a dedicated "Air"/HF-shimmer
  band beyond BassDecay's implicit high-band multiplier, true continuously-swappable
  early-reflection engines beyond the Space 3-way. These are M2+/M3 candidates, tracked as
  issues.

## Versioning

Ships as **v0.2.0** (breaking parameter changes are fine pre-1.0 — `size` and `bassDecay`
are new parameter IDs; state migration = tolerant import: v1 states load cleanly with the
two new parameters defaulting to 50%/130%, no crash on unknown/missing IDs, consistent with
the `AudioProcessorValueTreeState` tolerant-load behavior already relied on elsewhere in the
suite). CHANGELOG documents the early-reflection and multiband-decay model change
prominently as the headline v0.2.0 item.
