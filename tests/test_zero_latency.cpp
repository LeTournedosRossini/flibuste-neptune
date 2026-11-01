// Profil Live : chaque module à latence passe en variante à latence nulle sans toucher à ses réglages, puis retrouve ses réglages en profil Studio.
#include <catch2/catch_test_macros.hpp>

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
