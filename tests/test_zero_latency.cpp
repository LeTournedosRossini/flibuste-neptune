// Profil Live : chaque module à latence passe en variante à latence nulle sans toucher à ses réglages, puis retrouve ses réglages en profil Studio.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "neptune/core/CompressorModule.h"
#include "neptune/core/LimiterModule.h"
#include "neptune/core/OverdriveModule.h"
#include "neptune/core/ShaperModule.h"
#include "neptune/core/StockModule.h"

using namespace neptune::core;

namespace
{
void checkRoundTrip (const char* type, const std::vector<std::pair<ParamId, double>>& settings)
{
    auto inst = createInsert (type, 48000.0, 512);
    REQUIRE (inst != nullptr);
    auto& r = inst->runner;
    for (const auto& [id, v] : settings) r.params().set (id, v);
    const int studio = r.latencySamples();
    CAPTURE (type, studio);
    CHECK (studio > 0);
    r.setZeroLatency (true);
    CHECK (r.latencySamples() == 0);   // profil Live
    for (const auto& [id, v] : settings) CHECK (r.params().get (id) == v);   // les réglages de l'utilisateur ne bougent pas
    r.setZeroLatency (false);
    CHECK (r.latencySamples() == studio);   // retour en Studio : la même latence
}
}

TEST_CASE ("Profil Live : les modules à latence passent à latence nulle puis reviennent", "[latency][profile]")
{
    checkRoundTrip ("limiter", { { limiter::mode, limiter::kQuality }, { limiter::lookahead, 10.0 } });
    checkRoundTrip ("compressor", { { compressor::oversample, 2.0 }, { compressor::lookahead, 5.0 } });
    checkRoundTrip ("shaper", { { shaper::oversampling, 2.0 }, { shaper::lookahead, 3.0 } });
    checkRoundTrip ("overdrive", { { overdrive::oversampling, 2.0 } });
}

TEST_CASE ("Profil Live : un module sans latence reste à zéro", "[latency][profile]")
{
    auto inst = createInsert ("equalizer", 48000.0, 512);
    REQUIRE (inst != nullptr);
    CHECK (inst->runner.latencySamples() == 0);
    inst->runner.setZeroLatency (true);
    CHECK (inst->runner.latencySamples() == 0);
}

// Le profil Live force Zero Latency : le résultat doit être exactement celui du mode Zero Latency choisi à la main (même signal, mêmes mètres).
TEST_CASE ("Profil Live : Digue forcé donne exactement le mode Zero Latency choisi à la main", "[latency][profile]")
{
    auto make = [] (bool forced)
    {
        auto inst = createInsert ("limiter", 48000.0, 512);
        REQUIRE (inst != nullptr);
        auto& r = inst->runner;
        r.params().set (limiter::threshold, -18.0);
        r.params().set (limiter::ceiling, -1.0);
        r.params().set (limiter::mode, forced ? limiter::kQuality : limiter::kZeroLatency);
        r.setZeroLatency (forced);
        return inst;
    };
    auto a = make (true), b = make (false);
    std::vector<float> in (48000);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = 0.9f * std::sin (2.0 * 3.14159265358979 * 220.0 * static_cast<double> (i) / 48000.0) * (0.5f + 0.5f * std::sin (static_cast<double> (i) * 0.0003));
    double meterDiff = 0.0;
    for (std::size_t pos = 0; pos + 512 <= in.size(); pos += 512)
    {
        std::vector<float> al (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + 512)), ar = al, bl = al, br = al;
        a->runner.process (al.data(), ar.data(), 512, false);
        b->runner.process (bl.data(), br.data(), 512, false);
        CHECK (al == bl);
        meterDiff = std::max (meterDiff, static_cast<double> (std::abs (a->runner.module().meter (limiter::kReduction) - b->runner.module().meter (limiter::kReduction))));
    }
    CHECK (meterDiff == 0.0);
}
