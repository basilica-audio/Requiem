#include "dsp/IrAnalysis.h"
#include "dsp/ReverbEngine.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <vector>

TEST_CASE ("diag ned over time", "[.diag]")
{
    ReverbEngine engine;
    engine.setMixProportion (1.0f);
    engine.setPreDelayMs (0.0f);
    engine.setWidthPercent (100.0f);
    engine.setOutputDb (0.0f);
    engine.setModulationAmount (0.0f);
    engine.setDecaySeconds (3.0f);
    engine.setDampingHz (8000.0f);
    engine.setEarlyLateBalance (0.8f);
    engine.setEngineMode (ReverbEngine::EngineMode::hybridTail);
    engine.setTailModMode (FdnTail::ModulationMode::off);
    engine.prepare ({ 48000.0, 256, 2 });
    engine.regenerateImpulseResponseIfNeeded();
    engine.waitForPendingRender();

    const int total = 48000 * 2;
    std::vector<float> out ((size_t) total, 0.0f);
    juce::AudioBuffer<float> buf (2, 256);
    int w = 0;
    while (w < total) {
        int n = juce::jmin (256, total - w);
        buf.clear();
        if (w == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); }
        auto b = juce::dsp::AudioBlock<float> (buf).getSubBlock (0, (size_t) n);
        engine.process (b);
        for (int i = 0; i < n; ++i) out[(size_t) (w + i)] = buf.getSample (0, i);
        w += n;
    }

    std::printf ("mixing time %.4f s, branch delay %d smp\n",
                 engine.getMixingTimeSeconds(), engine.getHybridBranchDelaySamples());

    const int win = 1200;
    for (int ms = 20; ms <= 900; ms += 40)
    {
        const int start = (int) (ms * 48.0);
        if (start + win >= total) break;
        std::printf ("  t=%4d ms  NED=%.3f\n", ms, IrAnalysis::normalisedEchoDensity (out.data() + start, win));
    }
}
