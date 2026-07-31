#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "AttenuationDesigner.h"
#include "FdnTail.h"
#include "ImpulseResponseGenerator.h"
#include "IrAnalysis.h"
#include "MorphingConvolution.h"
#include "WetChain.h"

#include <mutex>

// The complete Requiem signal path, independent of juce::AudioProcessor so it
// can be exercised directly by unit tests without instantiating a full plugin
// (see tests/EngineTests.cpp). Owns all DSP state; every buffer/filter/delay
// line/convolution engine is allocated in prepare() and never reallocated on
// the audio thread.
//
// v0.3.0 signal flow (see docs/architecture.md for the full diagram):
//
//   input --> Pre-Delay --> [engine mode] --> Modulation (chorus, wet only)
//         --> Width (M/S, wet only) --> Wet chain (low/high cut + ducker)
//         --> Dry/Wet Mix (latency-compensated) --> Output trim
//
// with three engine modes:
//
//   Classic Convolution (the default, and bit-identical to v0.2.0)
//       the full procedural or user impulse response, convolved.
//
//   Hybrid Tail
//       the impulse response truncated at its analysed mixing time with a
//       10 ms raised-cosine fade supplies the early field, and a sixteen-line
//       FDN fitted to that same impulse response's per-octave RT60 supplies
//       the late field. The FDN branch is pre-delayed so its onset lands
//       exactly at the mixing time - see hybridBranchDelaySamples.
//
//   Tail Bloom
//       the full convolution, untouched, plus an FDN "bloom" layer summed on
//       top: a living, endlessly modulating tail over an authentic static
//       capture.
//
// THREADING. Unchanged in principle from the v0.1.1 fix, extended in scope.
// juce::dsp::Convolution's contract (JUCE 8.0.14) is that loadImpulseResponse()
// must be synchronised with process(), which in practice means calling it from
// the audio thread; that still holds - the audio thread is the only place a
// kernel is ever installed. What has moved is where the *work* happens: IR
// generation, analysis, and the FDN attenuation fit now run on a dedicated
// background thread ("Requiem IR Render") rather than on the message thread,
// so a 20 Hz parameter poll can never make the editor stutter. The message
// thread only posts parameter snapshots. Results reach the audio thread
// through SpinLock-guarded slots that process() only ever *tries*.
//
// reconfigureMutex (added for #29, mirroring basilica-audio/nave's
// CabConvolutionEngine::messageThreadMutex - see that repo's PR #28).
// prepare() is called from RequiemAudioProcessor::prepareToPlay(), which -
// like Nave's equivalent - the host may call from any thread it chooses;
// the VST3/AU contract guarantees only that it is not the audio thread, NOT
// that it is JUCE's own MessageManager thread. prepare() mutates
// sampleRate/numChannels/maximumBlockSize and fdnTail's delay-line buffers
// directly, and calls renderOnce() synchronously - all state the background
// "Requiem IR Render" thread's own renderOnce() call (see runRenderLoop())
// also reads/writes, with no synchronisation between the two pre-#29. Two
// non-audio threads touching the same mutable engine state with no lock is
// exactly Nave's #27/#28 bug shape, just one layer up from JUCE's
// Convolution itself. loadUserImpulseResponse()/clearUserImpulseResponse()
// have the identical hazard on usingUserImpulseResponse/
// userImpulseResponseFile: both are called from RequiemAudioProcessor::
// setStateInformation() (host thread, same "not guaranteed message thread"
// contract as prepareToPlay()) AND from GUI FileChooser callbacks (the real
// message thread) - see PluginProcessor.cpp. reconfigureMutex (a
// std::recursive_mutex - prepare() calls buildRequest()/renderOnce() while
// already holding it) is taken by prepare()'s entire body, by
// loadUserImpulseResponse()/clearUserImpulseResponse(), by buildRequest()
// (so every reader of usingUserImpulseResponse/userImpulseResponseFile is
// covered, including from regenerateImpulseResponseIfNeeded() on the real
// message thread), and around the renderOnce() call inside
// runRenderLoop() - serialising every one of those call sites against each
// other regardless of which OS threads they land on. Never taken by
// process()/applyPendingImpulseResponseIfAny()/
// applyPendingHybridSetupIfAny() (the audio-thread path), which keep their
// existing lock-free/SpinLock::ScopedTryLockType design - no lock or
// allocation is added to the audio thread. See
// tests/CrossThreadReprepareTests.cpp for the regression coverage and its
// audit-findings header comment for the full trace.
class ReverbEngine
{
public:
    // Order must match the Engine parameter's choice list in
    // ParameterLayout.cpp.
    enum class EngineMode
    {
        classicConvolution = 0,
        hybridTail = 1,
        tailBloom = 2,
    };

    ReverbEngine();
    ~ReverbEngine();

    // Allocates all DSP state and (re)generates the initial impulse response.
    // Must be called (and completed) before the first process() call, and
    // again whenever sample rate/block size/channel count change. Not
    // real-time safe; call only from the message thread.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears all state without deallocating. Safe on the audio thread.
    void reset();

    // Processes `block` in place. No allocation occurs here.
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    //==============================================================================
    // Real-time-safe parameter setters: smoothed (or internally ramped by the
    // owned juce::dsp objects), no allocation/locks.
    void setPreDelayMs (float newPreDelayMs);
    void setWidthPercent (float newWidthPercent);
    void setMixProportion (float newProportion01);
    void setOutputDb (float newOutputDb);
    void setModulationAmount (float newAmount01);

    // Real-time-safe to *call* - these only store the requested value in an
    // atomic. The impulse response is never regenerated here; that happens on
    // the IR render thread (see regenerateImpulseResponseIfNeeded()).
    void setDecaySeconds (float newDecaySeconds);
    void setDampingHz (float newDampingHz);
    void setSpaceType (ReverbIR::SpaceType newSpace);
    void setEarlyLateBalance (float newBalance01);
    void setFreeze (bool shouldFreeze);
    void setSize (float newSize01);
    void setBassDecayMultiplier (float newBassDecayMultiplier);

    //==============================================================================
    // v0.3.0 additions. All real-time safe to call.
    void setEngineMode (EngineMode newMode);
    EngineMode getEngineMode() const noexcept
    {
        return static_cast<EngineMode> (requestedEngineMode.load (std::memory_order_relaxed));
    }

    void setTailModMode (FdnTail::ModulationMode newMode);
    void setTailModDepth (float newDepth01);
    void setTailModRateScale (float newScale);
    void setBloomAmount (float newAmount01);

    void setLowCutHz (float newLowCutHz);
    void setHighCutHz (float newHighCutHz);
    void setDuckAmountPercent (float newDuckAmountPercent);
    void setDuckAttackMs (float newAttackMs);
    void setDuckReleaseMs (float newReleaseMs);

    //==============================================================================
    // Message-thread only. Posts the current parameter snapshot to the IR
    // render thread if anything that shapes the impulse response has changed
    // since the last render. A cheap no-op otherwise, so it is safe to call
    // from a ~20 Hz juce::Timer.
    void regenerateImpulseResponseIfNeeded();

    // Message-thread only. Blocks until the render thread has caught up with
    // the most recently posted snapshot, or until `timeoutMs` elapses.
    // Returns true if it caught up. Never call this from the audio thread.
    bool waitForPendingRender (int timeoutMs = 4000);

    // Message-thread only. Validates a user-supplied impulse-response file
    // and, if valid, hands it off for process() to load on the audio thread.
    bool loadUserImpulseResponse (const juce::File& file);

    // Message-thread only. Reverts to the procedural generator.
    void clearUserImpulseResponse();

    bool isUsingUserImpulseResponse() const noexcept { return usingUserImpulseResponse; }
    juce::File getUserImpulseResponseFile() const { return userImpulseResponseFile; }

    static constexpr double maxUserImpulseResponseSeconds = 30.0;

    // Reported latency, in samples. Zero in every mode: the convolution
    // engines are zero-latency/uniformly partitioned, and the correction FIR's
    // group delay is absorbed *inside* the FDN branch's own pre-delay budget
    // rather than delaying the output.
    int getLatencySamples() const noexcept { return latencySamples; }

    //==============================================================================
    // Introspection, for tests.
    MorphingConvolution& getMorphingConvolution() noexcept { return morphingConvolution; }
    const FdnTail& getFdnTail() const noexcept { return fdnTail; }
    const WetChain& getWetChain() const noexcept { return wetChain; }

    int getHybridBranchDelaySamples() const noexcept { return hybridBranchDelaySamples; }
    float getMixingTimeSeconds() const noexcept { return activeMixingTimeSeconds; }

    // True when the most recent analysis found the impulse response's decay
    // too far from exponential to splice confidently (gated reverbs, chopped
    // captures). Hybrid mode keeps the full convolution for such impulse
    // responses rather than splicing a tail that will not match.
    bool hasLowAnalysisConfidence() const noexcept { return lowAnalysisConfidence; }

private:
    //==============================================================================
    // Everything the render thread needs to reproduce a kernel. Compared as a
    // whole to decide whether a re-render is needed.
    struct RenderRequest
    {
        float decaySeconds = 2.5f;
        float dampingHz = 8000.0f;
        int space = static_cast<int> (ReverbIR::SpaceType::hall);
        float earlyLateBalance01 = 0.8f;
        bool freeze = false;
        float size01 = ReverbIR::defaultSize01;
        float bassDecayMultiplier = ReverbIR::defaultBassDecayMultiplier;
        int engineMode = static_cast<int> (EngineMode::classicConvolution);
        bool userIr = false;
        juce::File userIrFile;

        bool operator== (const RenderRequest& other) const noexcept;
        bool operator!= (const RenderRequest& other) const noexcept { return ! (*this == other); }
    };

    // Everything one render produces other than the convolution kernel.
    struct HybridSetup
    {
        bool valid = false;
        float mixingTimeSeconds = IrAnalysis::minMixingTimeSeconds;
        int branchDelaySamples = 0;
        bool lowConfidence = false;
        bool correctionFirActive = false;
        std::array<float, IrAnalysis::correctionFirLength> correctionFir {};
    };

    class IrRenderThread final : public juce::Thread
    {
    public:
        explicit IrRenderThread (ReverbEngine& ownerToUse)
            : juce::Thread ("Requiem IR Render"), owner (ownerToUse) {}

        void run() override { owner.runRenderLoop(); }

    private:
        ReverbEngine& owner;
    };

    void runRenderLoop();
    void renderOnce (const RenderRequest& request);
    RenderRequest buildRequest() const;
    void postRequest (const RenderRequest& request);

    void applyPendingImpulseResponseIfAny() noexcept;
    void applyPendingHybridSetupIfAny() noexcept;
    void applyWidth (juce::dsp::AudioBlock<float>& block) noexcept;
    void processCorrectionFir (juce::dsp::AudioBlock<float>& block, int numSamples) noexcept;

    static float mapModulationDepth (float amount01) noexcept;
    static float mapModulationMix (float amount01) noexcept;

    static constexpr double smoothingTimeSeconds = 0.05;
    static constexpr int maxPreDelaySamples = static_cast<int> (0.25 * 192000.0) + 4;

    // Branch pre-delay capacity: the mixing time is clamped to 350 ms, so
    // 400 ms at 192 kHz plus a margin covers every case.
    static constexpr int maxBranchDelaySamples = static_cast<int> (0.4 * 192000.0) + 8;

    static constexpr float modulationRateHz = 0.35f;
    static constexpr float modulationCentreDelayMs = 12.0f;

    // Engine-mode / branch-gain ramp, per brief 4.2.
    static constexpr double modeCrossfadeSeconds = 0.08;

    double sampleRate = 44100.0;
    int numChannels = 2;
    int maximumBlockSize = 512;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> preDelayLine { maxPreDelaySamples };
    MorphingConvolution morphingConvolution;
    juce::dsp::Chorus<float> modulationChorus;
    juce::dsp::Gain<float> outputGain;
    juce::dsp::DryWetMixer<float> dryWetMixer { 4096 };

    FdnTail fdnTail;
    WetChain wetChain;
    AttenuationDesign::DesignContext designContext;

    // FDN branch: pre-delay -> FDN -> correction FIR -> gain, summed into the
    // wet path.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> branchDelayLine { maxBranchDelaySamples };
    juce::AudioBuffer<float> branchBuffer;
    juce::AudioBuffer<float> dryMonoBuffer;

    // Hand-rolled 256-tap linear-phase FIR rather than juce::dsp::FIR: its
    // coefficients change whenever the analysis does, and swapping a
    // reference-counted Coefficients object on the audio thread risks
    // releasing the last reference - i.e. a free - inside process().
    std::array<float, IrAnalysis::correctionFirLength> correctionFirTaps {};
    std::vector<std::array<float, IrAnalysis::correctionFirLength>> correctionFirState;
    std::vector<int> correctionFirPositions;
    bool correctionFirActive = false;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> preDelayMsSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthAmountSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> branchGainSmoothed;

    float lastPreDelayMs = 20.0f;
    float lastWidthPercent = 100.0f;
    float lastMixProportion = 0.35f;
    float lastModulationAmount01 = 0.0f;

    std::atomic<float> requestedDecaySeconds { 2.5f };
    std::atomic<float> requestedDampingHz { 8000.0f };
    std::atomic<int> requestedSpace { static_cast<int> (ReverbIR::SpaceType::hall) };
    std::atomic<float> requestedEarlyLateBalance01 { 0.8f };
    std::atomic<bool> requestedFreeze { false };
    std::atomic<float> requestedSize01 { ReverbIR::defaultSize01 };
    std::atomic<float> requestedBassDecayMultiplier { ReverbIR::defaultBassDecayMultiplier };
    std::atomic<int> requestedEngineMode { static_cast<int> (EngineMode::classicConvolution) };
    std::atomic<float> requestedBloomAmount01 { 0.3f };

    bool usingUserImpulseResponse = false;
    juce::File userImpulseResponseFile;
    juce::AudioFormatManager userIrFormatManager;

    int latencySamples = 0;

    // Live splice state, owned by the audio thread.
    int hybridBranchDelaySamples = 0;
    float activeMixingTimeSeconds = IrAnalysis::minMixingTimeSeconds;
    bool lowAnalysisConfidence = false;

    //==============================================================================
    // Render-thread plumbing.
    enum class PendingImpulseResponseKind { none, procedural, userFile };

    struct PendingImpulseResponse
    {
        PendingImpulseResponseKind kind = PendingImpulseResponseKind::none;
        juce::AudioBuffer<float> proceduralBuffer;
        double proceduralSampleRate = 0.0;
        juce::File userFile;
        int padSamples = 0;
    };

    juce::SpinLock pendingImpulseResponseLock;
    PendingImpulseResponse pendingImpulseResponse;

    // Alternating trailing zero pad, assigned off the audio thread so
    // MorphingConvolution's install sentinel can never see two consecutive
    // kernels of equal length. Derived from the pad the audio thread last
    // *consumed*, so a kernel that gets coalesced away before consumption
    // never breaks the alternation.
    std::atomic<int> lastConsumedPadSamples { 1 };

    juce::SpinLock pendingHybridSetupLock;
    HybridSetup pendingHybridSetup;
    bool hasPendingHybridSetup = false;

    mutable juce::CriticalSection requestLock;
    RenderRequest requestedRender;
    RenderRequest lastRenderedRequest;
    bool hasRenderedOnce = false;

    // See the class-level THREADING comment above. Serialises prepare(),
    // loadUserImpulseResponse()/clearUserImpulseResponse(), buildRequest(),
    // and the renderOnce() call inside runRenderLoop() against each other.
    // Recursive: prepare() calls buildRequest()/renderOnce() while already
    // holding it. Never taken on the audio thread.
    mutable std::recursive_mutex reconfigureMutex;

    juce::WaitableEvent renderWakeUp;
    juce::WaitableEvent renderCompleted { true };

    IrRenderThread renderThread { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbEngine)
};
