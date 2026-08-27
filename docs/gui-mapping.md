# M3 photoreal GUI — "alchemie" design mapping

Requiem's M3 GUI reuses `basilica-audio/aureate`'s M3 pilot architecture
(a single baked master image, small live overlays, the preset-bar/scale-step
editor frame, and the `HubNeedle` component) against the **alchemie**
faceplate design (`brand/mocks/alchemie/`) — a moon-dial instrument, five
faceted crystal knobs, two buttons, and two dark glass windows on an
aubergine panel with silver engravings.

This document records the parameter-mapping decisions the briefing
delegated to this pass, the geometry measurements that back
`src/PluginEditorLayout.h`, and every place this design's own component
implementation diverges from the aureate/tubecomp pilot it started from.

## Knob mapping

| Knob slot | Position | Parameter | Ring? | Rationale |
|---|---|---|---|---|
| 0 | leftmost | Decay | yes (glow-knob-1) | Reverb time — the most load-bearing continuous control after Mix. |
| 1 | left-of-centre | Pre-Delay | yes (glow-knob-2) | Pairs with Decay as the "time" side of the layout. |
| 2 | **centre** (larger, octagonal) | **Mix** | **no** | The single most-reached-for control for a reverb's dry/wet blend — see "Centre knob feedback" below for why it has no ring. |
| 3 | right-of-centre | Damping | yes (glow-knob-3) | HF character of the tail — pairs with Size as the "space/tone" side. |
| 4 | rightmost | Size | yes (glow-knob-4) | Apparent room size, decoupled from Decay. |

Every other Requiem parameter (Width, Output, Space, Early/Late Balance,
Modulation, Bass Decay, and the whole v0.3.0 "Living Tail" set — Engine,
Tail Mod Mode/Depth/Rate, Bloom, Low Cut, High Cut, Duck/Attack/Release)
stays host-automatable and preset-controllable, but is **not** physically
knobbed this revision — the alchemie design has exactly 5 knob positions,
against Requiem's 21-parameter APVTS. This mirrors aureate's own M3 pilot
trade-off (10 physical knobs out of ~15 continuous parameters there).

### Centre knob feedback

The centre knob (larger, octagonal-faceted crystal, no ring sprite in
`glows.json`) drives Mix via the same invisible-hit-area pattern as the
other four, with `setPopupDisplayEnabled` for a value popup on drag —
**no additional visual feedback**. The briefing offered two options
("subtle brightness lift... derived at runtime from the master crop, or
popup-only"); popup-only was chosen because:

- No dedicated glow asset exists for the centre knob (unlike the other
  four, which have `glow-knob-{1..4}.png`).
- A hand-synthesised "brightness lift" derived at runtime from the crop
  risks exactly the "washed/pale light" defect the briefing explicitly
  flags as unacceptable, since it would not be grounded in the same
  registered-diff extraction pipeline the other lights use.
- Mix is still fully interactive, host-automatable, and accessible
  (`createAccessibilityHandler()` exposes its value) — it just has no
  bespoke photoreal lighting effect this revision.

## Button mapping

| Button | Position | Parameter | Visual feedback |
|---|---|---|---|
| Left | `buttonLeft1x` (117, 306) | **Freeze** (the only `AudioParameterBool` in Requiem's APVTS) | A minimal vector darken overlay (`fillEllipse` at 22% black alpha) drawn in `PluginEditor::paint()` when engaged. |
| Right | `buttonRight1x` (782, 306) | **IR override** — opens a `juce::PopupMenu` ("Load IR..." / "Use procedural IR") wired to the pre-existing, unmodified `RequiemAudioProcessor::loadUserImpulseResponseFile()`/`clearUserImpulseResponseFile()`/`isUsingUserImpulseResponse()` backend | Two independent overlays, same vector technique as Freeze: a transient pressed-state darken (22% black, `isDown()`), and a **persistent** subtle brightness lift (18% white) drawn whenever `isUsingUserImpulseResponse()` is true, so the override reads as "engaged" at a glance. |

Requiem's APVTS has exactly one boolean parameter (`freeze`); every other
parameter is a continuous float or a multi-choice combo, neither of which
fits the button metaphor. The right button therefore doesn't drive an APVTS
parameter at all — it is the IR-override entry point (see below), matching
the button count the alchemie design actually ships (two).

The Freeze button's pressed-state affordance is a plain vector overlay, not
an extracted asset, because the alchemie asset set (unlike tubecomp's
`toggle-N-down.png` family) does not ship a "button pressed" crop for
either button. Verified against the raw master crop (see
`docs/gui-preview.png`): the overlay measurably darkens the button (patch
mean RGB drops from ~(63,65,82) unlit to ~(41,43,49) engaged) while staying
subtle enough not to compete visually with the two additive light zones.
The IR override button's own two overlays reuse this exact technique
(`PluginEditor::repaintButtonZone()` factors the shared scaled-rect repaint
math both buttons' `onStateChange`/state-poll handlers use) — 22% black for
the transient press, and a separate 18% white lift for the persistent
"user IR active" marker, layered on top of it so a click while already
engaged still shows visible press feedback.

## IR override menu (right button)

Restores the pre-M3 editor's "Load IR..." / "Clear IR" file-chooser feature
— previously flagged in this document as removed with no GUI entry point —
through the reserved right button rather than dedicated controls, since the
alchemie design has only two physical buttons. `PluginEditor::showIrMenu()`
builds a two-item `juce::PopupMenu`:

- **"Load IR..."** — always enabled. Opens an async `juce::FileChooser`
  (`*.wav;*.aif;*.aiff`, `openMode | canSelectFiles`) via `launchAsync()`;
  on a result, hands the file straight to the processor's own
  `loadUserImpulseResponseFile()` (unchanged — still validates readable
  audio, `<=30s`, and gracefully no-ops on a bogus file; see
  `CLAUDE.md`'s DSP section).
- **"Use procedural IR"** — ticked when procedural is already the active
  source (`! isUsingUserImpulseResponse()`), and only *enabled* when a user
  IR is currently overriding it (`isUsingUserImpulseResponse()`) — i.e.
  disabled once procedural is already active, since there is nothing left
  to clear. Clicking it calls `clearUserImpulseResponseFile()`.

Both the menu itself (`PopupMenu::showMenuAsync`) and the file browse
(`FileChooser::launchAsync`) are async-only — no blocking `PopupMenu::show()`
or modal `FileChooser::browseFor...` call is used anywhere in this path,
per JUCE 8.0.14's own "no modal loops in plugin editors" guidance
(`juce_PopupMenu.h`/`juce_FileChooser.h`, this repo's pinned checkout at
`~/.cache/CPM/juce`), even though this CMake config sets
`JUCE_MODAL_LOOPS_PERMITTED=1` for other reasons.

The active/procedural state is not an APVTS parameter, so there is no
attachment to drive the lit-marker overlay from; `PluginEditor::paint()`
reads `isUsingUserImpulseResponse()` directly (always current), and the 30
Hz `timerCallback()` additionally polls it once per tick purely to decide
*when* to `repaint()` that zone — this catches the state changing for a
reason other than this button's own menu (e.g. a host reloading session
state, with a previously-saved user IR path, while the editor is already
open).

## Windows

Both dark glass windows (`windowLeft1x`, `windowRight1x` in
`PluginEditorLayout.h`, from `layout-manifest.json`'s `rectFeatures`) stay
dark/inert this revision, per the briefing. Reserved for a future
IR/decay-tail visualisation.

## Needle mapping

The needle displays `RequiemAudioProcessor::getCurrentOutputLevelDb()` — a
new, thread-safe atomic (mirroring `basilica-audio/aureate`'s
`currentGrDb`/`currentOutputLevelDb` pattern), written from `processBlock()`
via `buffer.getMagnitude(0, numSamples)` → `juce::Decibels::gainToDecibels`
(no allocation, real-time safe), read and ballistically smoothed on the GUI
side by `HubNeedle::tick()` at 30 Hz.

### Needle pivot verification

`components/needle.json`'s corrected pivot (695.00, 383.00 master px, the
2026-07-31 "hub centre, not rod end" fix) was verified by cropping
`master-01-base.png` around that exact point: it lands precisely on the
dark domed hub-cover baked into the master (the raised cap with a slotted
screw beneath it, at the base of the moon-dial). The needle sprite's own
opaque content terminates ~34px above its own canvas centre — i.e. the
rod's tail is meant to disappear behind that baked dome at every rotation
angle, never touch the pivot pixel itself. This confirms
`spriteIsPivotCentred: true` / `pivotXFrac`/`pivotYFrac` = 0.5 (the sprite's
own canvas centre IS the pivot) is the correct, current convention —
`pivotXInSprite`/`pivotYInSprite` (184, 184) in the same JSON are stale
values left over from before that fix and were not used.

### Needle sweep measurement

The alchemie moon-dial has **no printed numerals** — only lunar-phase and
planetary glyphs (crescent/gibbous moons, Mercury, Jupiter/"2", Mars,
Node/"N") around the arc, with plain tick dashes between them. Per the
briefing ("treat the arc between the extreme tick glyphs as the sweep
range"), the sweep was measured by overlaying a polar-protractor grid (2°
fine lines, 5° major lines, generated with a small throwaway Python/PIL
script) on `master-01-base.png`, centred on the dial's own measured centre
(`layout-manifest.json`'s `mainMeter`: cx=687.6, cy=286.5, r=184.7 master
px), and visually reading off the last engraved tick dash on each side
before the dial face disappears behind the baked hinge-cover dome:

- **Left extreme: -130°** (just past the Mars glyph, bottom-left)
- **Right extreme: +130°** (just past the Jupiter/"2" glyph, bottom-right)

Both measured clockwise from straight-up (12 o'clock), symmetric. This
±130° range (260° total sweep) is encoded in `src/gui/HubNeedle.cpp`'s
`ticks` table (now a 2-point linear range rather than aureate's 9-point
numeral-calibrated table — there are no intermediate numerals to fit
additional points to) and cross-checked in
`tests/gui/EditorLayoutTests.cpp`.

The dB range this sweep is mapped from (-60 dBFS → -130°, 0 dBFS → +130°)
is **this design's own convention**, not a measured quantity — Requiem's
output-level meter has no printed dB scale on the dial to calibrate
against. -60 dBFS was chosen as a practical "at rest / near-silent"
reference; 0 dBFS (full-scale) rests the needle at the clockwise extreme.

## Startup bezel-glow animation (signature behaviour #2)

Parameters (`src/PluginEditor.cpp`'s `easeOutBackWithOvershoot()`,
`src/PluginEditorLayout.h`'s `bezelStartupDurationSeconds`/
`bezelOvershootPeakT`):

- **Duration:** 1.2 s, timer-driven (30 Hz), runs once per editor
  instantiation (state is per-`RequiemAudioProcessorEditor` instance, not
  persisted — reopening the editor replays the power-up).
- **Curve:** a standard closed-form "ease-out-back" cubic,
  `f(x) = 1 + c3*(x-1)^3 + c1*(x-1)^2` with `c1 = 2.2`, `c3 = c1 + 1`. This
  satisfies `f(0) = 0` and `f(1) = 1` exactly, while rising above 1.0
  partway through and settling back down to exactly 1.0 by `x = 1` — no
  spring simulation/extra state needed.
- **Overshoot peak:** numerically verified (and cross-checked in
  `tests/gui/EditorLayoutTests.cpp`) at **~115.4%**, occurring at
  `x ≈ 0.542` (i.e. ~0.65 s into the 1.2 s window) — matching the owner's
  brief ("brief overshoot (~115%)") closely without hand-tuning a lookup
  table.
- **Compositing:** `AdditiveGlow` precomputes both the `t=1` "lit" frame
  and a `t=overshootGain` "overshoot" frame at construction, then
  cross-blends `base→lit` for `t∈[0,1]` and `lit→overshoot` for
  `t∈(1,overshootGain]` — **never** calling
  `juce::Graphics::setOpacity()` with a value outside `[0,1]`, which JUCE
  8.0.14's `Colour::withAlpha(float)` `jassert`s against. See
  `src/gui/AdditiveGlow.h`'s compositing-model docs for the full
  derivation (the two-stage blend is mathematically equivalent to
  `off + rgb*(alpha/255)*additiveGain*t`, clamped, at every `t`).
- Because the extracted glow sprite is, by construction,
  `(lit_registered - unlit)`, the `t=1` frame reconstructs the lit master
  pixel-for-pixel already — the standard `[0,255]` per-channel clamp
  *is* the "never exceed the lit master" hard ceiling at `t≤1`; the
  deliberate overshoot only pushes non-saturated pixels brighter, exactly
  as intended.

## Knob ring value display (signature behaviour #1)

Each of the four ring-lit outer knobs' `AdditiveGlow::drawWedge()` call
clips the precomputed `t=1` lit frame to an angular pie sector
(`juce::Path::addPieSegment`, 0° = 12 o'clock, clockwise-positive — matches
`glows.json`'s own angle convention) from that ring's own measured
`startAngleDeg` toward `startAngleDeg + (endAngleDeg-startAngleDeg) *
proportion`, where `proportion` is the knob's live normalised value
(`slider.valueToProportionOfLength(slider.getValue())`). This is
re-evaluated on every `onValueChange` (immediate, not polled), so the ring
sweeps continuously while dragging — no ballistic smoothing/idle-flicker,
per the briefing ("Continuous sweep, updates live while dragging").

### Ring-to-knob mapping

`glows.json`'s sprites measure their own ring centre independently from
`layout-manifest.json`'s knob-body centre (the ring halo sits outside the
crystal — radius ~66-72 master px vs. the knob body's own ~49-58 master px)
— cross-checked here to confirm each glow sprite's centre lands on the
correct knob:

| Glow sprite | centreX (master px) | Nearest knob (layout-manifest cx) | Δx |
|---|---|---|---|
| glow-knob-1.png | 255.38 | slot 0 (246.0) | 9.4 |
| glow-knob-2.png | 472.06 | slot 1 (459.0) | 13.1 |
| glow-knob-3.png | 902.57 | slot 3 (905.0) | 2.4 |
| glow-knob-4.png | 1118.95 | slot 4 (1122.0) | 3.1 |

(No sprite is close to slot 2's centre, 680.0 — confirming the centre knob
genuinely has no ring, matching the briefing's own description.)

## Component-reuse-vs-adaptation table

| Component | Source | Status |
|---|---|---|
| `src/gui/Flicker.h` | `aureate` (verbatim copy) | Copied for family completeness; **not wired** this revision — neither signature behaviour calls for idle breathing/flicker (the knob rings are a deterministic value display, the bezel glow is a one-shot animation, not a repeating idle effect). Flagged honestly rather than silently unused. |
| `src/gui/HubNeedle.{h,cpp}` | `aureate` (adapted) | Rotation maths, ballistics, and accessibility handler unchanged. Only the `ticks` table changed: 9-point numeral-calibrated table → 2-point linear range (alchemie's dial has no numerals — see "Needle sweep measurement" above). |
| `src/gui/AdditiveGlow.{h,cpp}` | **new** | alchemie's light model is ADDITIVE-over-unlit (opposite of aureate's SUBTRACTIVE-from-lit `SubtractiveGlow`, because the diff pair was registered onto the unlit frame — see `glows.json`'s own `registration` note). Structurally new, not an adaptation. |
| `src/gui/InvisibleKnob.h` | **new** | alchemie's crystal knobs don't rotate (no pointer on a faceted crystal — the ring IS the value display), so `MasterCropKnob`'s rotating-crop technique doesn't apply. Reuses `MasterCropKnob`'s own fine-drag (Shift = 8x) UX convention and minimal focus-ring affordance, but never touches its baked art. |
| `src/gui/MasterCropKnob.{h,cpp}` | `aureate` | **Not copied.** The knobs in this design never rotate — see `InvisibleKnob.h`. |
| `src/gui/ToggleZoneSwap.h` | `aureate` | **Not copied.** No pressed-state crop assets exist for alchemie's two buttons this revision (unlike tubecomp's `toggle-N-down.png` family) — see "Button mapping" above for the minimal vector alternative used instead. |
| `src/PluginEditor.{h,cpp}`, `src/PluginEditorLayout.h` | `aureate` (architecture reused, all geometry/content is alchemie-specific) | Single-master-image + overlay-components architecture, preset-bar/scale-step frame, and `applyScaleStep()`/`cycleScale()` pattern all copied; every geometric constant and every draw call's content is new/measured for this design. |
| `tests/gui/*` | `aureate` (architecture reused) | Same test categories (layout invariants, snapshot-with-live-state, a11y, component unit tests) adapted to alchemie's own geometry/behaviours, plus one new file (`AdditiveGlowTests.cpp`) for the new component, including a round-trip test against the real shipped assets (see its own top-of-file docs for the caveat about the raw, unregistered lit-reference file used as its fixture). |

## Measured provenance summary

- `brand/mocks/alchemie/layout-manifest.json` — plate/knob/button/window
  geometry (measured against `master-01-base.png`).
- `brand/mocks/alchemie/components/needle.json` — needle hub pivot
  (695.00, 383.00 master px, the 2026-07-31 corrected values) and rest
  angle (0°).
- `brand/mocks/alchemie/components/glows.json` — the 4 knob-ring and 1
  bezel additive-glow sprites' own canvas offset/size/centre/angle within
  the master, plus the additive compositing model and `additiveGain` (1.0).
- Needle sweep range (±130°) and its dB mapping — measured/decided for this
  pass, see "Needle sweep measurement" above (no JSON field covers this;
  the design has no printed numerals to calibrate against).

## Typography pass (suite typo phase)

Owner decision 2026-07-26: lettering is never AI-baked - it is set locally
as a sharp JUCE text layer in the suite serif (EB Garamond via BinaryData,
OFL). Implementation: `src/gui/PlateTypography.h` (copied verbatim from
the aureate pilot's typography pass), drawn last within `paint()` so no
glow/overlay blit can cover it.

What is lettered, all in SILVER (every baked engraving on this aubergine
plate is silver - the captions must read as part of that same engraving
system, not as gold additions):

- **Knob captions** `DECAY / PRE-DELAY / MIX / DAMPING / SIZE`, one row in
  the plate's bottom margin (the clean dark band UNDER the engraved
  scroll border, master y ~707..724) - the band directly under the knob
  bases is blocked by the border's central fleur ornament, and everything
  else around the knobs is baked vine/sigil relief.
- **Button captions** `FREEZE` (left bone button) and `IMPULSE` (right,
  the IR-loader menu), directly under each button.

The moon dial keeps its baked alchemical glyphs - they ARE this design's
scale, deliberately esoteric; no numerals are added there.

Tests: `tests/gui/EditorTypographyTests.cpp`.
