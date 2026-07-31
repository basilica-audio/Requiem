<p align="center"><img src="assets/icon.png" alt="Requiem icon" width="120"/></p>

# Requiem — user manual

*A cathedral in a box - cinematic convolution reverb for orchestral and choral space.*

## What Requiem is

Requiem is a convolution reverb built specifically for the orchestral/choral layer of a heavy-music mix - strings, choir, pads, ambient textures - rather than as a general-purpose reverb for every source. It generates its own impulse response procedurally (no bundled sample library to license or manage), shaped by controls that map to musically meaningful decisions: how big the space is, how bright or dark the tail sounds, how much of a distinct early "slap" it has versus a smooth wash, and whether it should sustain forever rather than decay. You can also load your own captured impulse response (a real cathedral, hall, plate, or anything else in WAV/AIFF/etc) if you want a specific, non-procedural space instead.

### v0.3.0: the Living Tail release

v0.3.0 adds a second way of making a reverb tail. Alongside the convolution engine Requiem has always had, there is now a **feedback delay network** whose decay is fitted automatically to whatever impulse response is loaded, hitting the target decay curve within 5% at every octave centre for the curve shapes Requiem produces (see [Under the hood](#under-the-hood) below) - and unlike a fixed impulse response, it never repeats. Three engine modes choose how the two are combined, and a wet-path EQ and ducker were added for mixing. Everything defaults to the v0.2.0 behaviour: an existing session sounds exactly as it did.

### v0.2.0: a research-derived voicing rework

Requiem's early-reflection and tail-darkening behavior was rebuilt in v0.2.0 against the *documented* design principles of category-defining cinematic/orchestral reverb units and general room-acoustics literature - public manuals, developer interviews, trade-press reviews, and DSP-literature articles, never against any hardware or commercial plugin's actual measured output, and no third-party impulse response was sampled or approximated (see `docs/research-notes.md` for the full sourcing and `docs/design-brief.md`'s Honesty section for what this does and doesn't license as a claim). The two headline corrections: early reflections now build up in *density* over the first tens of milliseconds instead of decaying from one loud initial tap, and the tail now darkens *progressively* as it decays (with bass ringing measurably longer than the highs) instead of applying one static filter color for its entire length. Two new controls - **Size** and **Bass Decay** - come out of that research pass; see their entries below.

## Where it sits in a heavy production chain

Heavy productions with orchestral elements typically separate the "aggressive" layer (rhythm guitars, drums, bass) from the "cinematic" layer (orchestra, choir, pads, ambience) so each can be processed and placed in the mix independently. Requiem is designed for the second layer:

- **Strings/orchestra bus**: a Hall or Cathedral space with a moderate Mix (30-50%) gives the orchestral layer room to breathe without smearing rhythmic detail. Use Pre-Delay to keep fast passages (spiccato, tremolo strings) intelligible - a bit of gap before the tail arrives preserves attack clarity.
- **Choir bus**: choir tends to want more reverb than instruments to sound "cathedral-scale" - try Cathedral space, a longer Decay, and a higher Mix than you'd use on strings. Damping pulled down slightly (a darker tail) keeps sibilance/breath noise from building up in the wash.
- **Ambient pads / transitions**: Freeze is built for this - hold a chord, engage Freeze, and let the frozen texture sustain under a transition or breakdown without needing a separate pad instrument.
- **Not recommended directly on**: distorted rhythm guitars or kick/snare - a short plate-style reverb or none at all usually serves those better; Requiem's cathedral/hall character will read as mud on fast, percussive, distorted sources. If you do want ambience on guitars, keep Mix low (10-20%) and Decay short.

A typical insert order on an orchestral/choir bus: EQ -> compression -> **Requiem** -> limiter (if used as the last stage on that bus). Requiem reports its own (normally zero) processing latency to the host, so it stays sample-accurately time-aligned with parallel dry buses if you're blending it in on an aux/send instead of an insert.

## Signal flow

```
input -> Pre-Delay -> Convolution (procedural or user IR) -> Modulation (chorus, wet only)
      -> Width (M/S, wet only) -> Dry/Wet Mix (latency-compensated) -> Output -> output
```

Decay, Damping, Space, Early/Late Balance, Freeze, Size, and Bass Decay shape the impulse response itself (regenerated in the background, not on every sample); Pre-Delay, Modulation, Width, Mix, and Output shape how that impulse response is applied to your signal in real time. See [`docs/architecture.md`](architecture.md) if you want the full technical explanation of why it's split this way.

### Reported latency

Requiem reports **0 samples of latency, always** - in all three Engine modes (Classic Convolution, Hybrid Tail, Tail Bloom), at every documented sample rate (44.1/48/96/192 kHz), and throughout a live Engine-mode switch or an in-progress IR morph. A Kronecker delta pushed through Classic Convolution lands its first output sample at index 0. This holds even in Hybrid Tail, where the synthesised FDN branch has its own internal onset delay (a linear-phase correction filter plus the network's own shortest delay line): that delay is compensated internally by the branch's own pre-delay rather than surfaced to the host, so the plugin never asks your DAW to account for it. Pre-Delay itself is not part of this - it's an audible, user-controlled gap between the direct sound and the tail's onset, not something the plugin hides via latency compensation.

## Parameter reference

### Engine

Chooses how the reverb tail is produced.

- **Classic Convolution** (default) - the engine Requiem has always used: one impulse response, convolved. Every existing session and preset uses this, and it sounds exactly as it did in v0.2.0.
- **Hybrid Tail** - the impulse response supplies only the early reflections, up to the point where the reflection pattern has become statistically indistinguishable from noise (the *mixing time*, measured automatically). From there a sixteen-line feedback delay network takes over, with its per-octave decay fitted to the impulse response's own measured decay. The result decays like the captured space but never loops, because nothing is being replayed.
- **Tail Bloom** - the full impulse response, untouched, with a modulating tail layered underneath it. Use this when you want a specific capture's character but want it to breathe.

Switching modes is click-free. It is not meant for continuous automation.

### Tail Mod Mode / Tail Mod Depth / Tail Mod Rate

Movement in the tail, for the two feedback-delay-network modes. Silent in Classic Convolution.

- **Matrix** (default) - modulates how the delay lines feed each other, using rotations that are mathematically guaranteed to preserve energy. Because no delay length ever changes, this adds movement **without detuning anything**: measured pitch deviation is under a hundredth of a semitone at full depth. Use this on anything harmonic.
- **Lush** - modulates the delay lengths themselves, the classic vintage approach. This *does* detune, audibly and deliberately. Use it when you want that character, and be careful with sustained tonal material.
- **Off** - no modulation.

Depth sets how much; Rate scales how fast, as a percentage of the built-in rates.

### Bloom

How much of the modulating tail is layered underneath the impulse response in **Tail Bloom** mode. Inaudible in the other two modes. The taper is deliberately gentle at the bottom of the range so low settings are usable rather than jumping straight to an obvious second layer.

### Low Cut / High Cut

A 12 dB/octave high-pass and low-pass on the **wet signal only** - the dry signal is never touched. Low Cut is the usual fix for a reverb that muddies the low end; High Cut takes the edge off a bright tail without dulling the source.

Both are hard-bypassed at their range ends (20 Hz and 20 kHz), which are the defaults. At those settings the filters are not run at all, so the wet path is bit-identical to v0.2.0.

### Duck / Duck Attack / Duck Release

Pulls the reverb down while the input is playing and lets it back up in the gaps - the standard trick for keeping a big reverb from burying a vocal or a dialogue track. The sidechain is the dry input, so what triggers the duck is the source, not the tail.

Duck is 0% by default, and at 0% the wet gain is exactly 1 - no processing happens at all. Attack and Release set how fast the duck engages and recovers, and are inert while Duck is 0%.

### Decay
**Range:** 0.1 – 10.0 s · **Default:** 2.5 s

How long the reverb tail takes to decay (RT60-style: the point at which it has dropped by 60 dB) - specifically, the *mid-frequency* reference rate the tail's low and high bands are measured relative to (see Bass Decay below). Short values (0.3-0.8 s) suit tight rooms/ambience; 1.5-3 s suits a concert hall; 4-10 s is cathedral/cavern territory, or useful as raw material for Freeze. Decay also sets the length of the generated impulse response, so very long Decay values cost more CPU (the convolution kernel is proportionally larger).

### Pre-Delay
**Range:** 0 – 250 ms · **Default:** 20 ms

The gap between the dry sound and the reverb tail's onset. A small amount (10-30 ms) is usually enough to keep a sense of "this reverb is separate from the direct sound" without sounding like a distinct slap-back. Larger values (60-150 ms) are useful for keeping fast rhythmic material (palm-muted guitars layered under the orchestra, staccato strings) tight and intelligible while the tail blooms in afterwards - the ear hears the attack clearly before the wash arrives.

### Damping
**Range:** 500 – 20000 Hz · **Default:** 8000 Hz

The tail's *terminal* high-frequency corner - as of v0.2.0, the tail darkens progressively as it decays (starting brighter and settling at this value by the time Decay finishes) rather than applying one static filter color for the tail's entire length, matching how real spaces darken over the reflection path (air absorption plus surface absorption compounding over time). Lower values produce a darker, more "absorbed" eventual tail color (heavy carpet/curtains, or just a duller-sounding space); higher values produce a brighter, more "hard surface" one (stone, glass). For choir and strings, pulling Damping down a bit from the default often reads as more natural and less fatiguing over a long mix, especially if the dry source is already bright.

### Space
**Choices:** Cathedral / Hall / Chamber · **Default:** Hall

Shapes the character of the early reflections layered ahead of the diffuse tail (see Early/Late Balance below) - this is what actually distinguishes "this sounds like a cathedral" from "this sounds like a small chamber," independent of Decay/Damping. As of v0.2.0 the early reflections build up in *density* over the first tens of milliseconds and hold roughly flat energy for a while longer, rather than decaying from one loud initial tap - see Size below for the continuous axis within each choice:

- **Cathedral**: the widest, longest buildup/handoff window and the densest tap budget - the sound of a large stone space with many nearby surfaces. Pairs well with long Decay and choir.
- **Hall**: a balanced, moderate window - the general-purpose default, good for strings and orchestra.
- **Chamber**: the tightest, shortest window - a small, intimate space. Good for a subtler sense of "this was played in a room" without an obviously large reverb.

### Size
**Range:** 0 – 100 % · **Default:** 50 %

The apparent size of the space, independent of both Decay (tail length) and Space (reflection character) - a continuous axis within whichever Space choice is selected. At 0% the early-reflection window/density is tighter (closer to a smaller room within that Space's own character); at 100% it's wider and denser (closer to a larger room). Sweeping Size does not change how long the reverb tail takes to decay - only how the space's apparent dimensions read before the diffuse tail takes over. Use it to fine-tune "how big does this Hall/Cathedral/Chamber actually feel" without touching Decay or reaching for a different Space choice.

### Bass Decay
**Range:** 25 – 175 % · **Default:** 130 %

How much longer (or shorter) the low end of the tail rings relative to the mid/high bands, as a percentage of Decay - matching how real halls very commonly let bass decay longer than the mids/highs (poor low-frequency absorption in most room materials). The default (130%) gives the low end a noticeably longer tail than the mids without swamping the mix; push it toward 175% for a dark, cavernous low-end bloom (pairs well with Freeze for an ambient pad), or pull it down toward 25% for a tighter, more controlled low end that won't build up mud under a busy arrangement. The mid band always tracks Decay directly; the high band always finishes somewhat before the mid band (not user-adjustable, matching the same real-hall HF-absorption principle).

### Early/Late Balance
**Range:** 0 – 100 % · **Default:** 80 %

Crossfades between the early-reflection layer (0%, shaped by Space) and the diffuse late tail (100%, shaped by Decay/Damping). At 0% you hear mostly the discrete early reflections - a short, direct character, closer to a slap-back or a small room's "liveness" than a wash. At 100% you hear a pure smooth diffuse wash with no distinct early character. The default (80%) keeps the diffuse tail dominant while still giving the early layer some presence - lower it if you want the Space setting's character to be more audible, raise it toward 100% for the smoothest, most "cinematic wash" result.

### Modulation
**Range:** 0 – 100 % · **Default:** 0 %

Adds a subtle, slow chorus-style movement to the reverb tail only (never to the dry signal). Procedurally generated impulse responses can occasionally sound slightly static or metallic compared to a real captured space; a small amount of Modulation (10-30%) softens that without being audible as an obvious chorus/vibrato effect. At 0% the Modulation stage is fully bypassed (identical output to not having it at all) - it's safe to leave at default unless you specifically want that extra movement.

### Freeze

**Off / On** · **Default:** off

**What Freeze does now depends on the Engine setting**, and the two behaviours are genuinely different:

- **In Classic Convolution** it works exactly as it did in v0.2.0 - described in full below. The tail is regenerated with a flat envelope, so the sustain is bounded by the Decay setting.
- **In Hybrid Tail and Tail Bloom** it is *structural and truly infinite*. The feedback delay network's per-line attenuation is faded out to unity over 20 ms, which leaves a lossless network: it holds the audio already circulating inside it, exactly, indefinitely. Nothing is regenerated, so the toggle takes effect within a single audio block rather than waiting for the next regeneration tick, and there is no Decay-length ceiling. Measured hold stability is within 0.2 dB over twenty seconds.

The rest of this section describes the Classic behaviour.

When engaged, the reverb tail sustains its current spectral content instead of decaying away - useful for holding a chord or texture under a transition, breakdown, or ambient section without needing a separate pad/drone instrument. Freeze is convolution-based, so the sustain is bounded by the Decay setting (up to 10 s), not literally infinite - think of it as "hold this snapshot of the tail for up to Decay seconds" rather than a feedback-loop-style infinite freeze. Damping still affects the frozen texture's brightness while it's engaged (held at one consistent color rather than continuing to darken); Early/Late Balance and the early-reflection layer are ignored while frozen (a frozen tail is always the full diffuse wash).

This finite-kernel design is a **deliberate choice, not a limitation**: research into feedback-loop-based "infinite reverb" designs documents that they progressively dull over time (repeated filtering in the feedback path continues attenuating highs even at unity feedback gain) and can develop audible periodicity as their internal diffusion/feedback order increases (see `docs/research-notes.md` section 4). Because Requiem's Freeze has no feedback path to filter repeatedly, it structurally cannot develop either artifact.

**Tip:** for a clean freeze moment, engage Freeze on a sustained chord (not mid-transient) and consider raising Decay first, since that determines how long the frozen kernel actually is.

### Width
**Range:** 0 – 200 % · **Default:** 100 %

Stereo width of the wet (reverb) signal only, via mid/side scaling - the dry signal's width is never touched. 0% collapses the wet signal to mono; 100% is the convolution engine's natural stereo image; up to 200% exaggerates it further for an especially wide, enveloping tail. Very wide settings (150-200%) can sound impressive in isolation but may cause phase/mono-compatibility issues - check your mix in mono if you push Width high.

### Mix
**Range:** 0 – 100 % · **Default:** 35 %

Dry/wet balance. At 0% Requiem is a transparent (latency-compensated) passthrough of the input - useful for A/B'ing the dry signal without removing the plugin, or when using Requiem on a send/aux bus where you want it fully wet at the plugin level and control blend via the send amount instead. The default (35%) suits a typical insert use on an orchestral/choir bus; push higher for a more ambient/washed-out result, or use 100% on a dedicated reverb return bus.

### Output
**Range:** -24 – 24 dB · **Default:** 0 dB

Trim applied after the dry/wet mix - use this to gain-stage the plugin's output level (e.g. after raising Mix significantly, or to match levels when A/B'ing different Decay/Space settings) without needing a separate gain plugin afterwards.

## Loading a custom impulse response

Use **Load IR...** in the editor to override the procedural generator with your own captured impulse response (WAV/AIFF). While a custom IR is loaded, Decay/Damping/Space/Early/Late Balance/Freeze/Size/Bass Decay no longer affect the sound (the loaded IR is used as-is); **Clear IR** reverts to the procedural generator, picking up whatever those controls are currently set to. The loaded IR's file path is saved with your session/preset; if the file has moved or been deleted when the session is reopened, Requiem falls back to the procedural generator rather than failing to load.

Requiem validates the file before loading it (rejecting anything it can't read as audio, or any file longer than 30 seconds - real captured impulse responses are essentially never that long, and this guards against accidentally selecting a full song/mix file instead of an actual IR).

## Under the hood

A few mechanisms worth knowing about if you want to understand why the Living Tail behaves the way it does, not just what the knobs are labelled:

**The decay curve is solved for, not dialled in.** Each of the sixteen FDN delay lines carries a ten-section octave-band graphic EQ plus a broadband gain, fitted to the impulse response's own measured per-octave decay through a Householder-QR pseudo-inverse and Gauss-Newton refinement against the realised response, inside a ±10 dB command-gain clamp. A final stability projection sweeps the finished cascade across the control-frequency grid and pulls the broadband gain down by exactly any excess, so no line's loop gain ever reaches unity at any frequency. The designed cascade lands within **5% of the target at every octave centre from 63 Hz to 8 kHz, on all sixteen lines**, for the curve shapes Requiem actually produces (a flat target, and the procedural generator's own analytic curve) - fifty additional randomised decay curves from 0.1 to 5 seconds were run the same way with zero failures. The solve is bit-reproducible: two plugin instances at the same settings can never drift apart.

**Movement without detuning, in Matrix mode.** The feedback matrix that drives Matrix-mode modulation is a Householder reflection composed with time-varying Givens rotations on disjoint index pairs, both orthogonal at every instant - so the network can't be destabilised by the modulation, and no delay length ever changes, meaning the modulation can't shift pitch. Measured pitch deviation at full depth: under one cent. Lush mode is the opposite tool, kept deliberately: it modulates the delay lengths themselves and detunes on purpose, which is the classic vintage-chorus character rather than a defect.

**The Hybrid Tail handover is measured, not guessed.** The point where the impulse response's reflection pattern becomes statistically indistinguishable from noise (the mixing time) is measured per impulse response rather than assumed, then clamped to a 50-350 ms window. The response is split there with a raised-cosine crossfade, and the FDN branch is pre-delayed by *two* things together - a short correction filter's group delay and the network's own shortest delay line - because compensating only the filter would place the synthesised tail's onset roughly 34 ms late at 48 kHz and leave an audible gap right after the early field ends. If an impulse response doesn't decay in a way the fit is confident about (a gated or heavily chopped capture, for instance), Hybrid Tail detects that and falls back to convolving the full, unmodified impulse response instead of splicing a bad fit onto it - silently, per impulse response, and by design rather than as a hidden failure.

**Every reverb-shaping knob crossfades, it never swaps hard.** Because kernel regeneration is asynchronous, changing Decay, Damping, Space, Size, Bass Decay or Early/Late Balance crossfades between the old and new convolution engines over 100 ms, gated on the new engine confirming it's actually ready rather than on a fixed timer - so a change is never applied before its kernel has finished loading.

**Freeze is genuinely unbounded, but only in the two FDN-based modes.** In Hybrid Tail and Tail Bloom, engaging Freeze fades the network's per-line attenuation out to unity over 20 ms, leaving a lossless loop that holds exactly what's already circulating inside it - indefinitely, and it takes effect within a single audio block rather than waiting for a regeneration tick. Measured hold stability: within ±0.2 dB over twenty seconds. In Classic Convolution, Freeze still works the way it always has - bounded by the Decay setting, up to 10 seconds - because a finite convolution kernel is what it is. That's a deliberate tradeoff rather than an oversight: a design with no feedback path to filter repeatedly structurally cannot develop the progressive high-frequency dulling or the audible periodicity that feedback-loop-style "infinite reverb" designs are documented to risk over long holds.

**Your existing sessions are unaffected, and that's tested, not just intended.** All ten new v0.3.0 parameters default to neutral, and the wet-path filters and ducker are *structurally* bypassed at their defaults (not merely multiplied by one) - a pre-v0.3.0 session loaded into v0.3.0 renders bit-identically to a fresh instance at the same settings, and the eleven pre-existing factory presets are unchanged.

**Engineering hygiene:** 134 test cases run on macOS and Windows on every push, plus pluginval at strictness 10 and `auval -strict` - covering zero heap allocations on the audio thread in all three engine modes (including mid-stream Freeze and mode switches), zero reported latency across the full sample-rate range, and a dedicated regression test for the kernel-reload race condition between the message thread and the audio thread.

## Presets

The preset bar at the top of the editor (`[<] [PresetName] [>] [Save] [Save As...] [Delete] [Import...] [Export...]`) gives you eleven factory starting points (see `docs/presets.md` for what each one is voiced for) plus your own saved presets, stored per-user at `~/Library/Audio/Presets/Yves Vogl/Requiem/` on macOS (`%APPDATA%/Yves Vogl/Requiem/Presets/` on Windows):

- **[<] / [>]** step through factory presets, then your own, alphabetically.
- **Clicking the preset name** opens a menu with Factory/User sections plus "Set current as default" (what a fresh plugin instance loads).
- **Save** overwrites the currently loaded user preset (disabled for factory presets - those are read-only); **Save As...** prompts for a new name.
- **Import.../Export...** read/write single `.basilicapreset` files, and Import also accepts `.zip` preset banks.
- A `*` after the preset name means the current settings have changed since it was loaded/saved.

The interface follows your system language automatically (English by default, German if your system language is set to German) - only interface labels/menus/dialogs are translated; parameter names and units always stay in English.

## Tips

- **Fast/rhythmic material under an orchestral wash**: raise Pre-Delay before reaching for a shorter Decay - it usually preserves clarity better while keeping the same overall sense of space.
- **Choir sounding harsh/sibilant in the tail**: lower Damping a few thousand Hz before reaching for an EQ on the reverb return.
- **"This reverb sounds a bit static/synthetic"**: try Modulation around 15-25% before assuming you need a different reverb entirely.
- **Building a pad/drone from an existing part**: automate Freeze on, ride Mix up, and consider a touch of Width and Modulation for movement while it holds.
- **Mono-compatibility check**: sum to mono periodically if you're running Width above ~150%, especially on a bus that might get folded to mono downstream (broadcast, some streaming platforms).
- **A space feels "too small" or "too big" for its Decay**: reach for Size before changing Decay or Space - it adjusts the apparent dimensions without touching how long the tail actually rings.
- **Low end building up mud in a dense mix**: pull Bass Decay down toward 100% or below (rather than shortening Decay overall, which would also shorten the mid/high tail you may still want).

## Known limitations (v0.3.0)

- **Hybrid Tail's echo density builds up gradually at the handover, not instantly.** The FDN branch is excited by an impulse, so its own reflection density climbs over the first several hundred milliseconds after the splice rather than starting dense - a known, measured characteristic of this release rather than a tuning bug, and it climbs steadily and predictably rather than sitting flat.
- **Hybrid Tail's spectral balance at the splice point matches within a few dB, not within one.** Matching it more tightly would mean rendering the whole network offline for every parameter change, which would break the fast, click-free re-solve that's the point of Hybrid mode - so the analytic route is used instead, at the cost of a small (single-digit dB) tilt right at the handover.
- **Matrix modulation's sidebands are audible by design, not suppressed to inaudibility.** They are the movement the feature exists to produce. What's held to a tight, tested bound is pitch stability (under one cent at full depth) - not the sidebands themselves.
- **No published CPU figure.** There is no CPU benchmark or CI performance gate in this project, so treat any CPU-usage number you see elsewhere as unverified. Long Decay values do cost proportionally more CPU and memory, since Decay also sets the generated kernel's length.
- **The v0.2.0 voicing is research-derived, not measured against hardware or a commercial plugin.** It's sourced from public manuals, developer interviews, trade-press reviews and room-acoustics literature; no hardware unit or commercial plugin's actual output was measured, and no third-party impulse response was sampled or approximated.
- **The procedural generator is a simplified model** - a filtered-noise-burst tail plus a discrete early-reflection train - rather than a physical simulation of a real room's modal behaviour or exact reflection geometry. It's built for a convincing cinematic wash, not for acoustic measurement accuracy.
- **The GUI is a functional slider/combo/toggle editor.** The custom vector-drawn GUI is a later milestone.
- **Pre-1.0, AGPLv3.** Breaking changes remain possible until v1.0.0.
