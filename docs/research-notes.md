# Requiem — Research Notes (deep-dive for design brief v2)

Reference class: cinematic/orchestral convolution & high-end algorithmic reverbs used on
scoring stages and orchestral/choral mix busses. Primary units researched: **Bricasti M7**
(the modern studio-standard algorithmic reverb, frequently A/B'd against real convolution),
**Lexicon 224 / 480L** (the historical hall/cathedral algorithms that *defined* the
"cinematic reverb" sound, designed by David Griesinger), **Altiverb** (the convolution
reverb built specifically around scoring-stage/hall impulse responses), plus DSP-literature
sources (LiquidSonics Reverberate multiband decay, general room-acoustics RT60 references)
and design commentary from Valhalla DSP on freeze/infinite-reverb pitfalls. No hardware or
commercial plugin was measured directly — all findings below are secondary-source (manuals,
developer interviews, trade-press reviews, DSP-literature articles). See Honesty section of
the brief for what this does and doesn't license us to claim.

## 1. Early reflections: density *builds up*, it does not just decay

Griesinger (designer of the Lexicon 224 and 480L), interviewed by Sound on Sound:

> "If it happens in that first 50mS, the apparent direction of the reflection is greatly
> influenced by the direction of the source."

> "Between 50mS and 120mS is probably the worst possible time to get energy from an
> intelligibility point of view."

Recommended shape: relatively **flat energy through ~160 ms** (not a smooth exponential
decay) — strong early energy pre-50ms can be "two or three times greater than the total
energy after 50mS," the 50-150ms band stays low/flat (this is the perceptually damaging
window for clarity — reflections here don't localize but do mask the direct sound), and
**only after ~160ms does the classic exponential decay take over** to produce the
sense of envelopment/hall reverberance.
[Sound on Sound — David Griesinger interview](https://www.soundonsound.com/people/david-griesinger-lexicon-creating-reverb-algorithms-surround-sound)

General principle (multiple sources): "in big rooms it takes more time for the reflections
to build up whereas in smaller rooms the initial echo density is higher" — i.e. **density
over time**, not amplitude over time, is the room-size cue. Reverb-design literature
describes implementing this as "an impulse response in which peak density increases with
time, simulating a continuously increasing reflections density."

**Gap vs Requiem v1:** `addEarlyReflections()` places tap 0 at sample 0 at full gain, then
scatters the remaining taps uniformly at random across a fixed window with **geometrically
decreasing amplitude per tap** (`tapDecayFactor` 0.85/0.75/0.65 for Cathedral/Hall/Chamber).
This is the opposite shape of what the research describes: real/well-regarded designs build
reflection *density* up over the first tens of milliseconds and keep the energy roughly flat
through ~160ms rather than letting a single early tap dominate and decay away geometrically
from sample 0. The tap counts (22/14/8) and window lengths (150/80/35 ms) are also
unsourced "felt right" numbers with no research grounding.

## 2. Bricasti M7 — three-engine split, and specifically a **sub-80Hz early-reflection engine**

Brian Zolner (Bricasti), quoted via Sound on Sound review:

> "One to handle the early reverberation, which most people term early reflections; one to
> cover the late decay tail; and a third that looks after the early reverberation below
> 80Hz."

The M7's early-reflection engine is deliberately dense/complex rather than exposing "very
obvious early reflections" the way some competitors do — reviewer: reflections are "very
dense and complex, making it sound closer to what happens in real life."

Also notable: decay is **not textbook-exponential** even in the tail region — "strong early
reflections decay to only a few percent of their original level before the reverb tail
starts to build up, and it is only in the later decay period that the decay curve starts to
look exponential" — i.e. there's a transition/handoff zone between early and late, not a
hard crossfade at a single balance point.
[Sound on Sound — Bricasti Design Model 7](https://www.soundonsound.com/reviews/bricasti-design-model-7)
[Bricasti M7 owner's manual (PDF)](https://www.bricasti.com/images/M7.pdf) — manual itself
confirms 12 parametric program parameters per algorithm including separate Pre-Delay,
Size (apparent size of the late field, distinct from Decay/RT), Early Reflections/Early
Select (build-up and decay of the early field), and independent HF/LF damping.

**Gap vs Requiem v1:** Requiem has no separate low-frequency early-reflection handling, no
"Size" parameter independent of Decay (Space's window/tap-count only changes with the
3-way Space choice, not continuously), and Early/Late Balance is a single equal-power
crossfade rather than a build-up/handoff transition.

## 3. Frequency-dependent (multiband) decay — highs decay faster than lows, and it's usually ≥2 independently-controllable bands, not one global LPF

Room-acoustics reference: "A room with RT60 = 0.8 s at 1 kHz might have RT60 = 2.0 s at
125 Hz if the materials absorb poorly at low frequencies" — i.e. real halls very commonly
have **longer low-frequency decay than mid/high**, on top of "most commonly, a reverb's high
frequencies decay faster than its low frequencies, because high frequencies are more easily
absorbed than low frequencies." Air absorption is a second, independent HF-shortening
mechanism in real large rooms: "air absorption at 4 kHz can reduce RT60 by 0.5 seconds or
more compared to what surface absorption alone predicts" in a concert hall.
[NTI Audio — RT60](https://www.nti-audio.com/en/applications/room-building-acoustics/reverberation-time) /
general RT60/room-acoustics literature.

LiquidSonics Reverberate 3.2 (multiband decay contouring feature) documents the standard
plugin-side implementation: "the reverb is split into three parts using high-quality
crossovers, and then each is passed into the re-decaying algorithm" with **independent decay
multipliers per band (25%-175% of the base decay time)** — example crossovers ~600-800 Hz
and ~6 kHz. Practical effect: "low-end extension creates a much darker reverb with prominent
bass decay," "high-end emphasis produces a lot of sizzle."
[LiquidSonics — Decay Time Adjustment and Contouring in Reverberate 3.2](https://www.liquidsonics.com/2021/09/29/decay-time-adjustment-and-contouring-in-reverberate-3-2/)

**Gap vs Requiem v1:** `Damping` is a *single* one-pole low-pass applied to the filtered
noise stream from `t = 0`, uniformly across the whole tail — it darkens the *entire* tail
equally rather than letting the tail get progressively darker as it decays (real HF
absorption + air absorption compound over time) and rather than giving bass its own,
typically-longer decay. There is no way in v1 to get a tail that starts bright and darkens
as it decays (the single-pole design applies the same filtered-noise character throughout;
only the RT60 envelope, not the spectral balance, changes over time).

## 4. Freeze / infinite sustain — the periodicity/dulling trap, and why convolution-based freeze avoids it

Valhalla DSP (ValhallaShimmer design notes) on feedback-loop-based infinite reverbs: "most
reverbs have lowpass filters in their feedback loops to create a more realistic decay, but
these lowpass filters can continue to filter away the high frequencies when the feedback
gain is at 0 dB, which results in the freeze reverb becoming duller and more muted over
several minutes." Feedback-delay/allpass-cascade freeze designs are also prone to
audible periodicity ("some weird spooky backwards thing") as diffusor order/feedback
increases.
[Valhalla DSP — ValhallaShimmer notes](https://www.valhalladsp.com/shimmer/ValhallaShimmerNotes.pdf)

**Assessment vs Requiem v1:** this is actually a point *in favor* of Requiem's existing
architecture, worth stating explicitly rather than "fixing": because Freeze is implemented
as a **finite, statically-generated convolution kernel** (envelope flattened to 1.0 across
the buffer, bounded to Decay seconds) rather than a feedback loop, it structurally cannot
develop the progressive HF-dulling or periodicity artifacts documented above — there is no
feedback path to filter repeatedly. The v2 brief should preserve this design and call it out
in the honesty/manual framing rather than replace it with a feedback-style freeze.

## 5. Predelay — 10-30ms is the documented default convention; Requiem's default (20ms) already matches

Multiple mixing-reference sources converge: "adding 10-30 ms of predelay makes the transient
arrive first, clean and clear, with the reverb filling in behind it" is described as a
standard convention; for orchestral/choir specifically, "around 25-30ms on the long side is
used to get that big hall feel," while "too little predelay causes instruments to lose
definition ... too much makes the effect sound disjointed, as if the reverb is arriving from
a completely separate environment." Source-distance layering convention: closer
instruments get more predelay, with strings often given more predelay than the source
signal itself to "make them speak more clearly," while choir is described as routed
differently (closer to "far reflections and tail").
[VI-Control — predelay discussion threads](https://vi-control.net/community/threads/when-should-i-use-predelay-on-reverb.113353/),
general mix-technique references.

**Assessment vs Requiem v1:** Decay/Pre-Delay ranges and the 20ms default are already
well-aligned with the literature — no change needed to the *range or default*; v2 should
just document the sourcing (currently the brief/manual states the default as an engineering
choice with no citation).

## 6. Altiverb — confirms "space" as a distinct axis, and that faithful early-reflection color includes low-frequency detail from the actual room

Altiverb's four-stage IR manipulation (Direct/Early/Tail controls) and its "Size" control —
"transposes room modes and resonances, tightens or spreads early reflections, and shortens
or lengthens the reverb tail" — again treats **Size as its own control axis**, separate
from decay time and separate from the coarse space-type choice. Altiverb's IR library is
built from real scoring stages (Air Studios, etc.) specifically because generic algorithmic
early reflections read as synthetic on orchestral sources — reinforcing why the
density-buildup and sub-80Hz-early-reflection findings above matter specifically for this
plugin's target material (orchestral/choral).
[Audio Ease — Altiverb](https://www.audioease.com/altiverb/)

## Sources consulted (full list)

- [Bricasti M7 owner's manual (PDF)](https://www.bricasti.com/images/M7.pdf)
- [Sound on Sound — Bricasti Design Model 7 review](https://www.soundonsound.com/reviews/bricasti-design-model-7)
- [Sound on Sound — David Griesinger (Lexicon) interview](https://www.soundonsound.com/people/david-griesinger-lexicon-creating-reverb-algorithms-surround-sound)
- [Valhalla DSP — Lexicon 224/480L history & design commentary](https://valhalladsp.wordpress.com/tag/lexicon-480l/)
- [DannyChesnut.com — Lexicon 224 program science/emulation notes](https://dannychesnut.com/Recording/Lexicon/224/index.html)
- [Audio Ease — Altiverb](https://www.audioease.com/altiverb/)
- [LiquidSonics — Decay Time Adjustment and Contouring in Reverberate 3.2](https://www.liquidsonics.com/2021/09/29/decay-time-adjustment-and-contouring-in-reverberate-3-2/)
- [NTI Audio — Reverberation Time Measurement (RT60)](https://www.nti-audio.com/en/applications/room-building-acoustics/reverberation-time)
- [Valhalla DSP — ValhallaShimmer design notes (PDF)](https://www.valhalladsp.com/shimmer/ValhallaShimmerNotes.pdf)
- [VI-Control — predelay/orchestral reverb technique threads](https://vi-control.net/community/threads/when-should-i-use-predelay-on-reverb.113353/)

## What this research does NOT license

No hardware unit or commercial plugin's actual output, IR, or internal DSP code was
measured, sampled, or reverse-engineered. Every number in the brief below is either (a) a
directly-quoted range/default from a public manual or interview, or (b) an explicitly-marked
"research-derived, reasoned" value where a source gives a principle but not an exact
number for Requiem's parameter space. No impulse responses from Altiverb's library (or any
other commercial IR library) were used or approximated.
