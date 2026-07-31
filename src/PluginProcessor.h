#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include "dsp/ReverbEngine.h"
#include "presets/PresetManager.h"

// Requiem: a cinematic convolution reverb. Signal flow lives in ReverbEngine
// (src/dsp) so it stays unit-testable independent of this AudioProcessor;
// this class is just APVTS + host plumbing + the message-thread impulse-
// response regeneration timer around it.
class RequiemAudioProcessor final : public juce::AudioProcessor,
                                     private juce::Timer
{
public:
    RequiemAudioProcessor();
    ~RequiemAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    // User impulse-response override (message-thread only - call these from
    // GUI code, e.g. a FileChooser callback, never from the audio thread).
    // Returns false, without changing any state, if the file doesn't exist.
    bool loadUserImpulseResponseFile (const juce::File& file);
    void clearUserImpulseResponseFile();
    bool isUsingUserImpulseResponse() const noexcept { return engine.isUsingUserImpulseResponse(); }
    juce::File getUserImpulseResponseFile() const { return engine.getUserImpulseResponseFile(); }

    // M3 GUI metering (src/gui/HubNeedle.h via PluginEditor): the
    // instantaneous post-engine output magnitude, written from
    // processBlock() as a plain atomic store (real-time safe - no
    // allocation/locks). Ballistic smoothing happens entirely on the GUI
    // side (HubNeedle::tick()), mirroring basilica-audio/aureate's
    // currentGrDb/currentOutputLevelDb pattern.
    float getCurrentOutputLevelDb() const noexcept { return currentOutputLevelDb.load (std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;

    // M2 preset system (.scaffold/specs/preset-system-m2.md). Declared
    // after apvts (construction order follows declaration order - see
    // PluginProcessor.cpp's makePresetManagerConfig()/
    // makeFactoryPresetAssets() helpers and docs/preset-system-notes.md,
    // ported verbatim from basilica-audio/nave's pilot implementation).
    // Public for PluginEditor's PresetBar to bind to, the same
    // "processor owns it, editor references it" pattern apvts itself
    // already uses.
    basilica::presets::PresetManager presetManager;

private:
    // juce::Timer callback: polls Decay/Damping for changes at a modest
    // rate and, off the audio thread, regenerates + loads a new procedural
    // impulse response if needed. See ReverbEngine::
    // regenerateImpulseResponseIfNeeded() and docs/architecture.md for why
    // this can't happen inside processBlock().
    void timerCallback() override;

    ReverbEngine engine;

    // See getCurrentOutputLevelDb() above. -100.0f is the same "effectively
    // silent" floor basilica-audio/aureate's own metering atomics use.
    std::atomic<float> currentOutputLevelDb { -100.0f };

    // Raw atomic pointers into the APVTS-managed parameter values, resolved
    // once at construction time so processBlock() never has to search for
    // them (no allocation/locks on the audio thread).
    std::atomic<float>* decaySeconds = nullptr;
    std::atomic<float>* preDelayMs = nullptr;
    std::atomic<float>* dampingHz = nullptr;
    std::atomic<float>* widthPercent = nullptr;
    std::atomic<float>* mixPercent = nullptr;
    std::atomic<float>* outputDb = nullptr;
    std::atomic<float>* spaceChoice = nullptr;
    std::atomic<float>* earlyLateBalancePercent = nullptr;
    std::atomic<float>* modulationPercent = nullptr;
    std::atomic<float>* freezeToggle = nullptr;
    std::atomic<float>* sizePercent = nullptr;
    std::atomic<float>* bassDecayPercent = nullptr;

    // v0.3.0 additions.
    std::atomic<float>* engineModeChoice = nullptr;
    std::atomic<float>* tailModModeChoice = nullptr;
    std::atomic<float>* tailModDepthPercent = nullptr;
    std::atomic<float>* tailModRatePercent = nullptr;
    std::atomic<float>* bloomAmountPercent = nullptr;
    std::atomic<float>* lowCutHz = nullptr;
    std::atomic<float>* highCutHz = nullptr;
    std::atomic<float>* duckAmountPercent = nullptr;
    std::atomic<float>* duckAttackMs = nullptr;
    std::atomic<float>* duckReleaseMs = nullptr;

    // Pushes every APVTS value into the engine. Real-time safe: each setter
    // either writes an atomic or updates an internally smoothed target.
    // Called from both prepareToPlay() (before prepare()) and processBlock().
    void pushParametersToEngine() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RequiemAudioProcessor)
};
