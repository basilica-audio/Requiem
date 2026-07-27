#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>

// Requiem v0.3.0 morphing convolution front-end: an A/B pair of
// juce::dsp::Convolution engines sharing one background message queue, with
// an equal-power *output* crossfade between them on every kernel change.
//
// WHY: JUCE's single-engine loadImpulseResponse() swaps the kernel outright,
// which is audible as a step whenever the IR changes - and Requiem's 20 Hz
// regeneration timer changes it up to twenty times a second while a knob is
// being dragged, producing the "staircase" this class exists to remove.
// Output crossfading is the artifact-free filter-exchange strategy (Wefers
// 2015, "Partitioned convolution algorithms for real-time auralization",
// sec. 4.5: output crossfading is artifact-free and simplest of the
// filter-exchange strategies; brief 3.1):
//
//     y[n] = cos(theta[n]) * y_old[n] + sin(theta[n]) * y_new[n]
//     theta: 0 -> pi/2, linear over 0.1 * fs samples
//
// READINESS HANDSHAKE (brief 3.1, mandatory). JUCE 8.0.14's
// juce_Convolution.h documents that loadImpulseResponse() is asynchronous -
// "The IR will become active once it has been completely loaded and
// processed, which may take some time" - that a pending engine only installs
// its new IR from inside process(), and that there is no completion
// callback. Starting the crossfade on load would therefore fade towards a
// stale (or silent) engine. Instead:
//
//   1. Warm-up: from the moment a kernel is posted, the idle engine is
//      processed every block (fed the same input, output discarded) so the
//      background-prepared IR can actually install. Outside the warm-up and
//      crossfade windows the idle engine is not processed at all - that is
//      what keeps steady-state CPU at single-engine cost.
//   2. Install detection via a unique-length sentinel: getCurrentIRSize() is
//      ambiguous when consecutive kernels share a length (a Damping-only
//      change never changes the procedural IR's length at all). Every
//      rendered kernel is therefore zero-padded with p in {0, 1} trailing
//      samples, alternating between successive *consumed* kernels, so two
//      kernels posted back to back can never report the same length. The
//      idle engine counts as ready on the first block where its
//      getCurrentIRSize() equals the length just posted. Trailing zeros are
//      numerically and audibly inert.
//
//      The alternation itself is the *producer's* job, not this class's:
//      padding here would mean resizing an AudioBuffer on the audio thread.
//      ReverbEngine's IR render thread applies the pad (see its
//      lastConsumedPadSamples atomic) and this class simply trusts that two
//      consecutively posted kernels differ in length.
//   3. Conservative fallback: if the sentinel has not matched after 500 ms of
//      *wall-clock* warm-up, the fade starts anyway (and asserts in debug
//      builds) - so a future JUCE change to IR-size reporting can never hang a
//      swap. Wall-clock, not a block count: an offline render pushes blocks
//      through far faster than real time, so a block-count deadline would
//      expire long before JUCE's background loader had any chance to install
//      the kernel. The fallback additionally refuses to fire while the idle
//      engine still reports no IR at all - fading towards a genuinely empty
//      engine would be a dropout, which is worse than the hard swap the
//      fallback exists to avoid.
//   4. theta only begins moving on the block *after* readiness; until then
//      the output is 100% the live engine, which preserves the steady-state
//      null invariant below.
//
// STEADY-STATE NULL INVARIANT: when no swap is in flight, process() copies
// the live engine's output through untouched - no crossfade multiply, no
// second engine - so the result is bit-identical to a plain single
// juce::dsp::Convolution chain. tests/MorphCrossfadeTests.cpp pins this.
//
// THREADING: kernels are handed in from the audio thread only (the caller
// owns the SpinLock-guarded hand-off slot that ReverbEngine has used since
// v0.1.1, satisfying juce::dsp::Convolution's own threading contract).
// canAcceptKernel() lets the caller leave a pending kernel in its slot while
// a crossfade is still running rather than dropping or buffering it here -
// no audio-thread allocation either way.
class MorphingConvolution
{
public:
    MorphingConvolution();

    // Not real-time safe. Prepares both engines; the currently loaded kernel
    // (see loadKernelSynchronously) stays active on the live engine.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears both engines and abandons any in-flight warm-up/crossfade
    // (the live engine keeps its kernel). Safe on the audio thread.
    void reset();

    // Processes `block` in place through the live engine, crossfading in the
    // idle engine when a swap is in flight. Never allocates.
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    //==============================================================================
    // Message-thread only, and only while processing is suspended (i.e. from
    // the owner's prepare()). Loads a kernel straight into the live engine
    // with no crossfade.
    void loadKernelSynchronously (juce::AudioBuffer<float>&& kernel, double kernelSampleRate,
                                   int numChannels);

    // Message-thread only, same contract as loadKernelSynchronously.
    void loadFileSynchronously (const juce::File& file, int numChannels);

    // Audio-thread only. Posts `kernel` to the idle engine and begins the
    // readiness handshake, using the buffer's own length as the install
    // sentinel. The buffer is moved in, so nothing is allocated here; the
    // caller is responsible for having applied the alternating 0/1-sample
    // zero pad off the audio thread (see the sentinel note above). Returns
    // false, changing nothing, if a swap is already in flight (see
    // canAcceptKernel).
    bool postKernel (juce::AudioBuffer<float>&& kernel, double kernelSampleRate, int numChannels) noexcept;

    // Audio-thread only. Posts a user IR file to the idle engine. File loads
    // cannot use the length sentinel (the post-load length is unknown until
    // JUCE has resampled/trimmed it), so readiness is detected as "the
    // reported IR size changed from what the idle engine held before the
    // load", still backed by the 500 ms fallback.
    bool postFile (const juce::File& file, int numChannels) noexcept;

    // True when a new kernel can be posted right now, i.e. no warm-up or
    // crossfade is in flight. Callers should leave a pending kernel in their
    // own hand-off slot while this is false.
    bool canAcceptKernel() const noexcept { return state == State::steady; }

    // True while the output crossfade itself is running (theta moving).
    bool isCrossfading() const noexcept { return state == State::crossfading; }

    // True while the idle engine is being warmed up but theta has not begun
    // moving yet.
    bool isWarmingUp() const noexcept { return state == State::warmingUp; }

    // Number of samples of crossfade completed so far, for tests.
    int getCrossfadeSamplesElapsed() const noexcept { return crossfadeSamplesElapsed; }

    int getLatency() const noexcept { return engines[0]->getLatency(); }

    // Length in samples of the kernel currently active on the live engine,
    // as reported by juce::dsp::Convolution itself.
    int getLiveIrSize() const noexcept { return engines[liveIndex]->getCurrentIRSize(); }

    //==============================================================================
    // Crossfade length, per brief 3.1.
    static constexpr double crossfadeSeconds = 0.1;

    // Conservative fallback margin: if the readiness sentinel has not been
    // observed within this long, the crossfade starts anyway.
    static constexpr double readinessFallbackSeconds = 0.5;

private:
    enum class State
    {
        steady,      // one engine live, the other suspended - bit-identical single-engine path
        warmingUp,   // idle engine processed (output discarded) until it reports the new kernel
        crossfading, // theta moving from 0 to pi/2
    };

    void beginWarmUp (int expectedSize) noexcept;

    double sampleRate = 44100.0;
    int maximumBlockSize = 512;
    int preparedChannels = 2;

    // A shared queue keeps both engines' background loading on one thread,
    // as juce::dsp::Convolution's own documentation recommends when several
    // instances are used together.
    juce::dsp::ConvolutionMessageQueue messageQueue;
    std::array<std::unique_ptr<juce::dsp::Convolution>, 2> engines;

    int liveIndex = 0;
    int idleIndex() const noexcept { return 1 - liveIndex; }

    State state = State::steady;

    // Expected getCurrentIRSize() on the idle engine once the posted kernel
    // has installed. -1 means "unknown" (a user file load), in which case
    // readiness is "the reported size changed and is non-zero".
    int expectedIdleIrSize = -1;
    int idleIrSizeBeforeLoad = 0;

    double warmUpStartedAtMs = 0.0;

    int crossfadeSamplesElapsed = 0;
    int crossfadeLengthSamples = 4410;

    // Scratch storage, allocated in prepare(): the block's input is
    // snapshotted into `idleOutput` before the live engine overwrites the
    // block in place, then the idle engine renders it there in place.
    juce::AudioBuffer<float> idleOutput;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphingConvolution)
};
