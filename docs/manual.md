<p align="center"><img src="assets/icon.png" alt="Requiem icon" width="120"/></p>

# Requiem — user manual

*A cathedral in a box - cinematic convolution reverb for orchestral and choral space.*

## What Requiem is

Requiem is a convolution reverb built specifically for the orchestral/choral layer of a heavy-music mix - strings, choir, pads, ambient textures - rather than as a general-purpose reverb for every source. It generates its own impulse response procedurally (no bundled sample library to license or manage), shaped by controls that map to musically meaningful decisions: how big the space is, how bright or dark the tail sounds, how much of a distinct early "slap" it has versus a smooth wash, and whether it should sustain forever rather than decay. You can also load your own captured impulse response (a real cathedral, hall, plate, or anything else in WAV/AIFF/etc) if you want a specific, non-procedural space instead.

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

## Parameter reference

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
