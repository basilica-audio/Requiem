# Factory presets

Seventeen factory presets ship with Requiem, embedded via BinaryData from
`presets/factory/*.json` (see `docs/architecture.md`'s "M2 preset system and
i18n frame" section for the build wiring). All are sourced starting points
from `docs/design-brief.md`'s "Factory Presets" section - see that document's
own Honesty section for what these numbers are and aren't calibrated
against (research/manual/interview-derived, not measured against any
commercial hardware or software).

| Preset | Category | Intent |
|---|---|---|
| **Default** | Init | The plugin's out-of-the-box state (every parameter at its `ParameterLayout` default), exposed as an explicit preset so there's always a one-click way back to the baseline voicing. Also this plugin's out-of-the-box default (see the M2 default-resolution order in `docs/architecture.md`). |
| **Cathedral Wash** | Orchestral | Long, dark, enveloping - the default "big choir/strings" starting point (Cathedral, Size 85%, Decay 6.5s, Bass Decay 150%). |
| **Concert Hall** | Orchestral | The general-purpose orchestral-bus default (Hall, Size 50%, Decay 2.2s) - close to v1's factory defaults, now spectrally correct per the v0.2.0 multiband-decay rework. |
| **Chamber Room** | Orchestral | Intimate, small-ensemble character (Chamber, Size 25%, Decay 1.0s). |
| **Choir Bloom** | Choir | Wide early-reflection spread and a dark Damping setting (tames sibilance per `docs/manual.md`'s tip), with extra Pre-Delay for choir-bus clarity (Cathedral, Size 90%, Decay 5.0s, Pre-Delay 35ms). |
| **Tight Rhythmic Hall** | Orchestral | Fast material stays intelligible thanks to a longer Pre-Delay (90ms) rather than a shorter Decay (Hall, Size 40%, Decay 1.6s). |
| **Frozen Drone** | Ambient | Ambient pad/transition-hold starting point: Freeze on, generous Bass Decay (175%), a touch of Modulation and Width for movement. |
| **Dark Sanctuary** | Ambient | Pure diffuse wash (Early/Late Balance 100%, no distinct early character), very long and dark (Cathedral, Size 100%, Decay 9s, Bass Decay 175%). |
| **Bright Slap Chamber** | Orchestral | Distinct, audible early reflections - the "small room liveness" `docs/manual.md` describes (Chamber, Size 15%, Decay 0.6s, Early/Late Balance 20%). |
| **Full Wet Send Hall** | Bus | Dedicated aux/return-bus use case (Mix 100%) at v1's global default Decay (2.5s) and the new global default Bass Decay (130%), as a nod to continuity. |
| **Living Cathedral** | Orchestral | *(v0.3.0)* The flagship Hybrid Tail starting point: a long, dark cathedral whose late field is synthesised and therefore never repeats (Hybrid, Cathedral, Decay 7.5s, Size 90%, Matrix modulation at 45%, wet-path Low Cut 80 Hz to keep the low end clear). |
| **Breathing Chamber** | Rooms | *(v0.3.0)* A small, live room with visible movement in the tail - the Hybrid mode's answer to the "static/synthetic" problem at short decay times (Hybrid, Chamber, Decay 1.6s, Matrix modulation at 55% and 140% rate). |
| **Blooming Hall** | Orchestral | *(v0.3.0)* Tail Bloom over a conventional hall: the full impulse response untouched, with a modulating layer swelling underneath it (Bloom 45%, Hall, Decay 3.4s). |
| **Vintage Lush Plate** | Character | *(v0.3.0)* Deliberately detuned. Lush modulation moves the delay lengths themselves, the classic vintage approach, at 65% depth - use it for character, not for sustained tonal material (Hybrid, Decay 2.8s, band-limited 150 Hz to 9 kHz). |
| **Dialogue-Ducked Score** | Utility | *(v0.3.0)* A large scoring reverb that gets out of the way of dialogue: Duck 65% with an 8 ms attack and a 400 ms release, plus a wet-path band limit (Hybrid, Cathedral, Decay 4.2s). |
| **Infinite Frozen Nave** | Special | *(v0.3.0)* Freeze in an FDN mode is structural and genuinely unbounded, unlike the Classic engine's Decay-limited hold. This preset ships frozen, with a heavy bloom layer and slow modulation, as a sustained drone (Bloom 70%, Freeze on, Mix 70%). |
| **Subtle Air** | Orchestral | Barely-there ambience for sources that just need a hint of space (Mix 12%), with a touch of Modulation to avoid the "static/synthetic" character `docs/manual.md`'s tips warn about. |

Every factory preset's parameters were designed against `docs/design-brief.md`'s
"Factory Presets" section and encoded by this rework's implementation pass;
none reference a user-loaded impulse-response file (loading one is always a
separate, explicit user action - see `docs/manual.md`'s "Loading a custom
impulse response" section).
