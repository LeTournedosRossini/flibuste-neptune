// Compensation de latence de Neptune (profil Studio) : un insert à latence sur une piste ne décale plus cette piste par rapport aux autres.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

#include "neptune/core/Engine.h"
#include "neptune/core/LimiterModule.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"
#include "neptune/core/StockModule.h"

using namespace neptune::core;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::shared_ptr<const Instrument> constantInstrument (float level)
{
    auto s = std::make_shared<SampleData>();
    s->channels = 1;
    s->frames = 480000;
    s->sampleRate = kRate;
    s->pcm32.assign (static_cast<std::size_t> (s->frames), level);
    auto inst = std::make_shared<Instrument>();
    inst->zones.push_back (makeFullKeyboardZone (std::move (s), 60, PlayMode::Loop));
    inst->finalize();
    return inst;
}

struct Rig
{
    ParameterSet params;
    Engine engine;
    std::vector<std::vector<float>> bufs;
    std::vector<float*> ptrs;

    Rig()
    {
        REQUIRE (sampler::addParams (params));
        REQUIRE (probe::buildPool (params, mixer::numTracks));
        REQUIRE (mixer::addParams (params, mixer::numTracks, mixer::numBuses));
        engine.attachParameters (&params);
        engine.prepare (kRate, kBlock);
        bufs.assign (16, std::vector<float> (kBlock, 0.0f));
        for (auto& b : bufs) ptrs.push_back (b.data());
    }
    // Rend la voie gauche de la première sortie sur `blocks` blocs.
    std::vector<float> render (int blocks)
    {
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b)
        {
            engine.process (ptrs.data(), static_cast<int> (ptrs.size()), kBlock);
            out.insert (out.end(), bufs[0].begin(), bufs[0].end());
        }
        return out;
    }
    void omni()   // les deux premières pistes écoutent tous les canaux MIDI
    {
        for (int t = 0; t < 2; ++t) engine.setTrackSettings (t, mixer::StripSettings {});
    }
    void addLimiter (int track)   // Digue en Quality, 10 ms : 480 échantillons à 48 kHz
    {
        auto ins = createInsert ("limiter", kRate, kBlock);
        REQUIRE (ins != nullptr);
        ins->runner.params().set (limiter::lookahead, 10.0);
        engine.setInsert (StripKind::Track, track, 0, ins);
    }
};

std::size_t firstAbove (const std::vector<float>& x, float threshold)
{
    for (std::size_t i = 0; i < x.size(); ++i) if (std::abs (x[i]) > threshold) return i;
    return x.size();
}
} // namespace

TEST_CASE ("compensation : deux pistes au Master, une avec un insert à latence, arrivent ensemble en Studio", "[latency][engine]")
{
    Rig r;
    r.engine.setProfile (PerformanceProfile::Studio);
    r.engine.publishInstrument (0, constantInstrument (0.1f));
    r.engine.publishInstrument (1, constantInstrument (0.1f));
    r.omni();
    r.addLimiter (0);
    const int total = r.engine.updateLatencyPlan();
    CHECK (total == 480);
    CHECK (r.engine.latencySamples() == 480);
    r.engine.process (r.ptrs.data(), 16, kBlock);   // adopte instruments, insert et plan
    r.engine.noteOn (0, 60, 1.0f);
    const auto out = r.render (20);
    // rien avant la latence totale, puis les deux pistes ensemble (une seule marche, pas deux)
    CHECK (firstAbove (out, 0.001f) >= static_cast<std::size_t> (total) - 1);
    const float plateau = out[4000];
    CHECK (plateau > 0.2f);   // 0,1 + 0,1 x gain de repos de Digue (+2,7 dB) : les deux pistes sonnent
    CHECK (out[static_cast<std::size_t> (total) + 8] > 0.9f * plateau);
}

TEST_CASE ("compensation : en profil Live, rien n'est retardé et la latence annoncée est nulle", "[latency][engine]")
{
    Rig r;
    r.engine.setProfile (PerformanceProfile::Live);
    r.engine.publishInstrument (0, constantInstrument (0.1f));
    r.engine.publishInstrument (1, constantInstrument (0.1f));
    r.omni();
    r.addLimiter (0);
    CHECK (r.engine.updateLatencyPlan() == 0);
    r.engine.process (r.ptrs.data(), 16, kBlock);
    r.engine.noteOn (0, 60, 1.0f);
    const auto out = r.render (20);
    CHECK (firstAbove (out, 0.001f) < 4);   // Digue passe en Zero Latency et rien n'est compensé : le son part tout de suite
    CHECK (out[4000] > 0.2f);
}

TEST_CASE ("compensation : la latence est la même par un bus que par la sortie principale", "[latency][engine]")
{
    Rig r;
    r.engine.setProfile (PerformanceProfile::Studio);
    r.engine.publishInstrument (0, constantInstrument (0.1f));
    r.engine.publishInstrument (1, constantInstrument (0.1f));
    r.addLimiter (0);
    r.omni();
    mixer::StripSettings s;
    s.routing.mainDest = 1;   // la piste 1 passe par le bus 1, la piste 2 va au Master
    r.engine.setTrackSettings (0, s);
    const int total = r.engine.updateLatencyPlan();
    CHECK (total == 480);
    r.engine.process (r.ptrs.data(), 16, kBlock);
    r.engine.noteOn (0, 60, 1.0f);
    const auto out = r.render (20);
    CHECK (out[4000] > 0.2f);
    CHECK (firstAbove (out, 0.001f) >= static_cast<std::size_t> (total) - 1);
    CHECK (out[static_cast<std::size_t> (total) + 8] > 0.9f * out[4000]);
}
