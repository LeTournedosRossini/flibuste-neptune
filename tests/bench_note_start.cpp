// Coût d'un début de note dans le cœur de Neptune : temps du bloc qui contient le noteOn, comparé aux blocs sans événement.
// Exécutable séparé, hors ctest. Sert à chercher le pic de calcul signalé par le banc (docs/neptune-instants-des-notes.md, section 5.1).
//   neptune_bench_note_start [blocs] [--froid]  (défaut 64 échantillons ; --froid : la mémoire cache est vidée avant chaque bloc)
#include "neptune/core/Engine.h"
#include "neptune/core/Instrument.h"
#include "neptune/core/SamplerParams.h"
#include "neptune/core/Synth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using neptune::core::Engine;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kChannels = 64;

struct Stats { double median = 0, p99 = 0, max = 0; };
Stats stats (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return { v[v.size() / 2], v[static_cast<std::size_t> (static_cast<double> (v.size()) * 0.99)], v.back() };
}

void report (const char* name, int block, const std::vector<double>& steady, const std::vector<double>& start)
{
    const double budget = 1.0e6 * block / kSampleRate;   // µs
    const auto a = stats (steady), b = stats (start);
    std::printf ("%-26s bloc %4d  sans note : méd %7.1f µs  p99 %7.1f  max %7.1f | avec début de note : méd %7.1f µs  p99 %7.1f  max %7.1f | budget %.0f µs  (max note = %.0f %%)\n",
                 name, block, a.median, a.p99, a.max, b.median, b.p99, b.max, budget, 100.0 * b.max / budget);
}

enum class Kind { Sampler, SynthSaw, SynthUnison };

std::vector<char> g_thrash (96u << 20);   // vidé de la mémoire cache avant chaque bloc mesuré avec --froid : le DAW fait travailler d'autres plugins entre deux notes
volatile char g_sink = 0;

void run (Kind kind, const char* name, int block, int notesAtOnce, bool cold)
{
    Engine engine;
    neptune::core::ParameterSet params;
    neptune::core::sampler::addParams (params);
    engine.attachParameters (&params);
    engine.prepare (kSampleRate, block);
    std::vector<std::vector<float>> storage (kChannels, std::vector<float> (static_cast<std::size_t> (block), 0.0f));
    std::vector<float*> chans (kChannels);
    for (int c = 0; c < kChannels; ++c) chans[static_cast<std::size_t> (c)] = storage[static_cast<std::size_t> (c)].data();

    if (kind == Kind::Sampler)
    {
        auto sample = std::make_shared<neptune::core::SampleData>();
        sample->channels = 2;
        sample->frames = static_cast<std::int64_t> (2.0 * kSampleRate);
        sample->sampleRate = kSampleRate;
        sample->pcm32.resize (static_cast<std::size_t> (sample->frames) * 2);
        for (std::int64_t i = 0; i < sample->frames; ++i)
        {
            const float v = 0.3f * std::sin (6.2831853f * 220.0f * static_cast<float> (i) / static_cast<float> (kSampleRate));
            sample->pcm32[static_cast<std::size_t> (i)] = v;
            sample->pcm32[static_cast<std::size_t> (sample->frames + i)] = -v;
        }
        auto inst = std::make_shared<neptune::core::Instrument>();
        inst->zones.push_back (neptune::core::makeFullKeyboardZone (std::move (sample), 60, neptune::core::PlayMode::OneShot));
        engine.publishInstrument (std::move (inst));
    }
    else
    {
        neptune::core::SynthDescription d;
        d.maxVoices = 16;
        d.filters.filter[0].cutoffHz = 3000.0;
        if (kind == Kind::SynthUnison)
        {
            d.osc[0].unison = 8;
            d.osc[0].curve = neptune::core::DetuneCurve::Supersaw;
            d.filters.filter[1].on = true;
        }
        engine.setTrackType (0, neptune::core::TrackType::Synth);
        engine.publishSynth (0, std::make_shared<neptune::core::SynthPatch> (d));
    }
    engine.process (chans.data(), kChannels, block);   // adopte l'instrument ou le patch
    for (int i = 0; i < 300; ++i) engine.process (chans.data(), kChannels, block);

    std::vector<double> steady, start, onCall;
    const int period = static_cast<int> (0.125 * kSampleRate / block);   // une note toutes les 125 ms, comme le projet du banc
    int key = 48;
    for (int i = 0; i < 4000; ++i)
    {
        const bool noteBlock = i % period == 0;
        if (noteBlock)
        {
            engine.panic();
            const auto n0 = Clock::now();
            for (int n = 0; n < notesAtOnce; ++n) engine.noteOn (0, key + 3 * n, 0.8f);
            onCall.push_back (std::chrono::duration<double, std::micro> (Clock::now() - n0).count());
            key = key >= 72 ? 48 : key + 1;
        }
        if (cold) { for (std::size_t k = 0; k < g_thrash.size(); k += 64) g_thrash[k] = static_cast<char> (k + static_cast<std::size_t> (i)); g_sink = g_thrash[static_cast<std::size_t> (i) % g_thrash.size()]; }
        const auto t0 = Clock::now();
        engine.process (chans.data(), kChannels, block);
        const double us = std::chrono::duration<double, std::micro> (Clock::now() - t0).count();
        (noteBlock ? start : steady).push_back (us);
    }
    report (name, block, steady, start);
    const auto c = stats (onCall);
    std::printf ("%-26s            appel noteOn seul : méd %7.1f µs  p99 %7.1f  max %7.1f\n", "", c.median, c.p99, c.max);
}
} // namespace

int main (int argc, char** argv)
{
    const int block = argc > 1 ? std::atoi (argv[1]) : 64;
    const bool cold = argc > 2 && std::string (argv[2]) == "--froid";
    run (Kind::Sampler, "Sampler, 1 note", block, 1, cold);
    run (Kind::Sampler, "Sampler, accord de 4", block, 4, cold);
    run (Kind::SynthSaw, "Synthé, 1 note", block, 1, cold);
    run (Kind::SynthSaw, "Synthé, accord de 4", block, 4, cold);
    run (Kind::SynthUnison, "Synthé unisson 8, 1 note", block, 1, cold);
    run (Kind::SynthUnison, "Synthé unisson 8, accord 4", block, 4, cold);
    return 0;
}
