#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "ImpulseResponseGenerator.h"

// The complete Requiem signal path, independent of juce::AudioProcessor so
// it can be exercised directly by unit tests without instantiating a full
// plugin (see tests/EngineTests.cpp). Owns all DSP state; every buffer/
// filter/delay-line/convolution/chorus engine is allocated in prepare() and
// never reallocated on the audio thread.
//
// Signal flow (see docs/architecture.md for the full diagram and the
// latency-compensation rationale):
//
//   input -> Pre-Delay -> Convolution (wet) -> Modulation (chorus, wet only)
//         -> Width (M/S, wet only) -> Dry/Wet Mix (latency-compensated)
//         -> Output trim
//
// The impulse response driving the Convolution stage is generated
// procedurally off the audio thread (see ImpulseResponseGenerator.h) from
// the Decay/Damping/Space/Early-Late-Balance/Freeze parameters, or - if a
// user IR has been loaded - read from an audio file. Decay/Damping/Space/
// Early-Late-Balance/Freeze changes only ever update an atomic "requested
// value"; the actual (re)generation happens on the message thread, inside
// regenerateImpulseResponseIfNeeded()/loadUserImpulseResponse(), which the
// owning PluginProcessor calls from a message-thread juce::Timer/FileChooser
// callback - never from process(). This is the "ROBUSTNESS first" v0.1
// approach called out in the DSP spec: no attempt is made to interpolate/
// crossfade between old and new IRs beyond whatever juce::dsp::Convolution
// itself does internally when a new IR is loaded while processing
// continues.
//
// Threading (JUCE 8.0.14 juce_dsp/frequency/juce_Convolution.h, "Threading"):
// "It is not safe to interleave calls to the methods of this class. If you
// need to load new impulse responses during processing the load() calls
// must be synchronised with process() calls, which in practice means making
// the load() call from the audio thread." Accordingly,
// juce::dsp::Convolution::loadImpulseResponse() is called *only* from
// process() (the audio thread) here - regenerateImpulseResponseIfNeeded()/
// loadUserImpulseResponse()/clearUserImpulseResponse() only ever prepare a
// request (generating the procedural buffer, or validating a candidate user
// IR file - both real work, neither real-time safe) and hand it off through
// a SpinLock-guarded slot (see applyPendingImpulseResponseIfAny()) for
// process() to pick up and load on its own thread, wait-free, on the next
// call. The one exception is prepare() (called from prepareToPlay(), always
// with processing suspended per the host/JUCE contract - there is no
// concurrent process() call to race), which still loads directly.
class ReverbEngine
{
public:
    ReverbEngine();

    // Allocates all DSP state and (re)generates the initial impulse
    // response (procedural, from the last commanded parameters, or the
    // active user IR file if one was loaded before this call). Must be
    // called (and completed) before the first process() call, and again
    // whenever sample rate/block size/channel count change. Not real-time
    // safe - allocates and does file/generation work; call only from the
    // message thread (e.g. AudioProcessor::prepareToPlay).
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears all filter/delay-line/convolution/chorus state without
    // deallocating. Safe to call from the audio thread (e.g. on playback
    // stop/loop).
    void reset();

    // Processes `block` in place. `block` must have at most the maximum
    // sample/channel counts declared to prepare(); a zero-sample block is a
    // safe no-op. No allocation occurs here.
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    //==============================================================================
    // Real-time-safe parameter setters: smoothed (or internally ramped by
    // the owned juce::dsp objects), no allocation/locks. Safe to call every
    // block from the audio thread.
    void setPreDelayMs (float newPreDelayMs);
    void setWidthPercent (float newWidthPercent);
    void setMixProportion (float newProportion01);
    void setOutputDb (float newOutputDb);

    // Depth of the post-convolution Modulation (chorus) stage applied to
    // the wet tail only, in [0, 1]. 0 leaves the chorus's internal dry/wet
    // mix at 0 (bit-identical passthrough of its input); real-time safe -
    // forwards directly to juce::dsp::Chorus's own internally smoothed
    // parameters every block, the same pattern used for Mix/DryWetMixer.
    void setModulationAmount (float newAmount01);

    // Real-time-safe to *call* - these only store the requested value in an
    // atomic. No allocation, no impulse-response regeneration happens here;
    // that only occurs inside regenerateImpulseResponseIfNeeded() (message
    // thread only, see below). Safe to call every block from the audio
    // thread (e.g. so the processor can always forward the current APVTS
    // value without needing to know which thread it's on).
    void setDecaySeconds (float newDecaySeconds);
    void setDampingHz (float newDampingHz);
    void setSpaceType (ReverbIR::SpaceType newSpace);
    void setEarlyLateBalance (float newBalance01);
    void setFreeze (bool shouldFreeze);

    // Message-thread only. Regenerates a new procedural impulse response
    // (off the audio thread - allocates, does per-sample generation work)
    // if any of Decay/Damping/Space/Early-Late-Balance/Freeze have changed
    // since the last generated IR, and no user IR override is currently
    // active, and hands it off to process() to actually load into
    // juce::dsp::Convolution on the audio thread (see the class comment's
    // "Threading" section). A cheap no-op otherwise, so it is safe to call
    // frequently (e.g. from a ~20 Hz juce::Timer in PluginProcessor).
    void regenerateImpulseResponseIfNeeded();

    // Message-thread only. Validates a user-supplied impulse-response audio
    // file (WAV/AIFF/etc, anything juce::AudioFormatManager's basic formats
    // support) and, if valid, hands it off to process() to actually load
    // into juce::dsp::Convolution on the audio thread (see the class
    // comment's "Threading" section), overriding the procedural generator
    // until clearUserImpulseResponse() is called. Returns false, without
    // changing any state, if the file doesn't exist, isn't readable as
    // valid audio (a format-reader sanity check runs before anything is
    // handed to juce::dsp::Convolution), or exceeds
    // maxUserImpulseResponseSeconds - guarding against a pathologically
    // long "IR" file driving convolution CPU/memory far outside what a real
    // captured space would ever need.
    bool loadUserImpulseResponse (const juce::File& file);

    // Message-thread only. Reverts to the procedural generator, discarding
    // any active user IR override, and queues a fresh procedural IR the
    // same way regenerateImpulseResponseIfNeeded() does.
    void clearUserImpulseResponse();

    bool isUsingUserImpulseResponse() const noexcept { return usingUserImpulseResponse; }
    juce::File getUserImpulseResponseFile() const { return userImpulseResponseFile; }

    // User-supplied impulse-response files longer than this are rejected by
    // loadUserImpulseResponse() rather than loaded - generous enough for
    // any real captured space (cathedral IRs are rarely more than a few
    // seconds), while bounding the convolution engine's worst-case CPU/
    // memory cost against a mis-selected non-IR audio file.
    static constexpr double maxUserImpulseResponseSeconds = 30.0;

    // Latency reported by the convolution engine (samples), valid after
    // prepare() has run. juce::dsp::Convolution's default configuration
    // (used here) is zero-latency/uniformly partitioned, so this is
    // normally 0, but it is queried rather than assumed so the plugin stays
    // correct if that ever changes. The Modulation (chorus) stage adds no
    // reported latency - it is a short, continuously modulated delay, not a
    // bulk delay, and is treated the same way a hardware chorus/vibrato
    // effect would be (see docs/architecture.md).
    int getLatencySamples() const noexcept { return latencySamples; }

private:
    // Message-thread only (called from prepare(), where there is no
    // concurrent process() call to race - see the class comment's
    // "Threading" section). Generates the procedural IR and loads it into
    // convolution directly.
    void loadProceduralImpulseResponseSynchronously (float decaySeconds, float dampingHz,
                                                       ReverbIR::SpaceType space, float earlyLateBalance01, bool freeze);

    // Message-thread only. Generates the procedural IR (the non-real-time-
    // safe part) and hands it off via pendingImpulseResponse for process()
    // to actually load, on the audio thread.
    void queueProceduralImpulseResponse (float decaySeconds, float dampingHz,
                                          ReverbIR::SpaceType space, float earlyLateBalance01, bool freeze);

    // Audio-thread only, called once at the top of every process(). Applies
    // (i.e. calls convolution.loadImpulseResponse()) whatever request the
    // message thread most recently queued via queueProceduralImpulseResponse()
    // /loadUserImpulseResponse(), if any - this is the only place
    // convolution.loadImpulseResponse() is ever called outside prepare(),
    // satisfying juce::dsp::Convolution's own threading contract (see the
    // class comment above). Never blocks: uses a try-lock, so if the
    // message thread happens to be mid-update (a vanishingly short window -
    // see pendingImpulseResponseLock's doc comment) this simply tries again
    // on the very next block, still far faster than the ~20 Hz cadence
    // requests arrive at.
    void applyPendingImpulseResponseIfAny() noexcept;

    void applyWidth (juce::dsp::AudioBlock<float>& block) noexcept;

    static float mapModulationDepth (float amount01) noexcept;
    static float mapModulationMix (float amount01) noexcept;

    static constexpr double smoothingTimeSeconds = 0.05;
    // 250 ms at up to 192 kHz, plus a small margin - sized once so
    // setMaximumDelayInSamples() never needs to be called again (that call
    // is not real-time safe) regardless of host sample rate.
    static constexpr int maxPreDelaySamples = static_cast<int> (0.25 * 192000.0) + 4;

    // Fixed Modulation (chorus) character - only its depth/mix are exposed
    // as the user-facing Modulation parameter; rate/centre-delay/feedback
    // are chosen for a subtle, non-seasick "richness" effect rather than an
    // obvious chorus/flanger and are not automatable.
    static constexpr float modulationRateHz = 0.35f;
    static constexpr float modulationCentreDelayMs = 12.0f;

    double sampleRate = 44100.0;
    int numChannels = 2;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> preDelayLine { maxPreDelaySamples };
    juce::dsp::Convolution convolution;
    juce::dsp::Chorus<float> modulationChorus;
    juce::dsp::Gain<float> outputGain;

    // Sized generously above any realistic convolution latency (the default
    // zero-latency configuration reports 0) so setWetLatency() never
    // exceeds the mixer's internal delay-line capacity.
    juce::dsp::DryWetMixer<float> dryWetMixer { 4096 };

    // Pre-Delay is a filter-coefficient-style parameter (recomputing the
    // delay-line's sample count is cheap, but modulating a fractional delay
    // continuously per-sample is not needed here), so - like Overture's
    // Tight/Tone frequencies - it is smoothed and re-applied once per block.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> preDelayMsSmoothed;
    // Width is a plain per-sample multiply once resolved to a scalar per
    // block, smoothed the same way to avoid zipper noise on automation.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthAmountSmoothed;

    // Last commanded values (ParameterLayout defaults until a setter is
    // called), re-applied to the smoothers/DSP objects on every prepare()
    // so re-prepare (sample-rate change, etc.) never resets a live
    // parameter back to a default.
    float lastPreDelayMs = 20.0f;
    float lastWidthPercent = 100.0f;
    float lastMixProportion = 0.35f;
    float lastModulationAmount01 = 0.0f;

    // Decay/Damping/Space/Early-Late-Balance/Freeze: the "requested" value
    // is just an atomic, safe to write from any thread; the "last
    // generated" value is message-thread-only state used to detect changes
    // in regenerateImpulseResponseIfNeeded().
    std::atomic<float> requestedDecaySeconds { 2.5f };
    std::atomic<float> requestedDampingHz { 8000.0f };
    std::atomic<int> requestedSpace { static_cast<int> (ReverbIR::SpaceType::hall) };
    std::atomic<float> requestedEarlyLateBalance01 { 0.8f };
    std::atomic<bool> requestedFreeze { false };

    float lastGeneratedDecaySeconds = -1.0f; // forces generation on first prepare()
    float lastGeneratedDampingHz = -1.0f;
    ReverbIR::SpaceType lastGeneratedSpace = ReverbIR::SpaceType::hall;
    float lastGeneratedEarlyLateBalance01 = 0.8f;
    bool lastGeneratedFreeze = false;

    bool usingUserImpulseResponse = false;
    juce::File userImpulseResponseFile;

    // Used only by loadUserImpulseResponse() to sanity-check a candidate
    // file (readable audio, sane length) before handing it to
    // juce::dsp::Convolution - message-thread only, never touched from
    // process().
    juce::AudioFormatManager userIrFormatManager;

    int latencySamples = 0;

    // See "Threading" in the class comment above. The message thread only
    // ever holds pendingImpulseResponseLock for a short, non-allocating
    // struct/pointer swap (the expensive work - procedural generation,
    // file-format validation - happens before the lock is taken); the audio
    // thread only ever *tries* the lock, once per process() call, and never
    // blocks. This is exactly the pattern juce::dsp::Convolution's own
    // AudioBuffer-taking loadImpulseResponse() overload recommends for
    // handing a buffer to the audio thread without allocating there: "...
    // use some wait-free construct (a lock-free queue or a SpinLock/
    // GenericScopedTryLock combination) to transfer ownership to the audio
    // thread without allocating."
    enum class PendingImpulseResponseKind
    {
        none,
        procedural,
        userFile,
    };

    struct PendingImpulseResponse
    {
        PendingImpulseResponseKind kind = PendingImpulseResponseKind::none;
        juce::AudioBuffer<float> proceduralBuffer;
        double proceduralSampleRate = 0.0;
        juce::File userFile;
    };

    juce::SpinLock pendingImpulseResponseLock;
    PendingImpulseResponse pendingImpulseResponse;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbEngine)
};
