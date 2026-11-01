// Diapason comme insert : le générateur remplace le signal de l'emplacement, le bypass le laisse passer.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "neptune/core/DiapasonModule.h"
#include "neptune/core/StockModule.h"

using namespace neptune::core;

namespace
{
float peak (const std::vector<float>& x)
{
    float p = 0.0f;
    for (const float v : x) p = std::max (p, std::abs (v));
    return p;
}
}

TEST_CASE ("Diapason en insert : crée le module et produit un signal continu", "[diapason][insert]")
{
    auto inst = createInsert ("diapason", 48000.0, 512);
    REQUIRE (inst != nullptr);
    CHECK (inst->type == "diapason");
    auto& p = inst->runner.params();
    p.set (generator::signal, generator::kSine);
    p.set (generator::mode, generator::kContinuous);
    p.set (generator::on, 1.0);
    std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
    for (std::size_t pos = 0; pos < l.size(); pos += 512) inst->runner.process (l.data() + pos, r.data() + pos, 512, false);
    CHECK (peak (l) > 0.01f);
    CHECK (peak (r) > 0.01f);
}

TEST_CASE ("Diapason en insert : bypass laisse passer l'entrée, arrêt = silence", "[diapason][insert]")
{
    auto inst = createInsert ("diapason", 48000.0, 512);
    REQUIRE (inst != nullptr);
    auto& p = inst->runner.params();
    p.set (generator::mode, generator::kContinuous);
    p.set (generator::on, 1.0);
    std::vector<float> l (512), r (512);
    for (int block = 0; block < 6; ++block)   // le bypass se fait en fondu : on mesure après quelques blocs
    {
        std::fill (l.begin(), l.end(), 0.25f);
        std::fill (r.begin(), r.end(), 0.25f);
        inst->runner.process (l.data(), r.data(), 512, true);
    }
    CHECK (l[100] == Catch::Approx (0.25f).margin (1e-6));
    p.set (generator::on, 0.0);
    std::vector<float> l2 (512), r2 (512);
    for (int block = 0; block < 8; ++block)
    {
        std::fill (l2.begin(), l2.end(), 0.5f);
        std::fill (r2.begin(), r2.end(), 0.5f);
        inst->runner.process (l2.data(), r2.data(), 512, false);
    }
    CHECK (peak (l2) < 0.01f);   // le signal d'entrée est remplacé par le générateur arrêté
}
