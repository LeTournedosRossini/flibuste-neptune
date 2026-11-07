#include <catch2/catch_test_macros.hpp>

#include "neptune/core/Control.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/ParamGroups.h"
#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"

using namespace neptune::core;
using namespace neptune::core::paramgroups;

TEST_CASE ("param groups: scope and family come from the parameter name")
{
    auto g = classify ("Track 3 Osc 2 Fine");
    CHECK (g.scope == Scope::Track);
    CHECK (g.index == 2);
    CHECK (g.family == Family::Synth);

    g = classify ("Track 12 Send 4 Level");
    CHECK (g.index == 11);
    CHECK (g.family == Family::Sends);

    CHECK (classify ("Track 1 Volume").family == Family::Strip);
    CHECK (classify ("Track 1 Insert 3 Bypass").family == Family::Inserts);
    CHECK (classify ("Track 1 FM Op 2 Level").family == Family::Fm);
    CHECK (classify ("Track 1 Upper Drawbar 1").family == Family::Organ);
    CHECK (classify ("Track 1 Slot 4 Level").family == Family::Zones);
    CHECK (classify ("Track 1 Modulator 2 Rate").family == Family::Modulators);

    g = classify ("Parallel 9 Send 2 Level");
    CHECK (g.scope == Scope::Parallel);
    CHECK (g.index == 8);
    CHECK (classify ("Bus 2 Insert 1 Bypass").scope == Scope::Bus);

    g = classify ("Master Insert 1 Bypass");
    CHECK (g.scope == Scope::Master);
    CHECK (g.family == Family::Inserts);
    CHECK (classify ("Master Volume").family == Family::Strip);

    // la piste 1 a ses réglages d'instrument sans préfixe
    g = classify ("Filter 1 Cutoff");
    CHECK (g.scope == Scope::Track);
    CHECK (g.index == 0);
    CHECK (g.family == Family::Filters);
    CHECK (classify ("Gain").family == Family::Sampler);

    CHECK (classify ("Macro 3").scope == Scope::Global);
    CHECK (classify ("Macro 3").family == Family::Other);
    CHECK (classify ("").family == Family::Other);
    CHECK (classify ("Track").scope == Scope::Global);
}

TEST_CASE ("param groups: every real parameter lands in a family, except the macros")
{
    ParameterSet set;
    REQUIRE (probe::buildPool (set, 4));
    REQUIRE (sampler::addParams (set));
    REQUIRE (mixer::addParams (set, 4, mixer::numBuses));
    REQUIRE (control::addMacroParams (set));
    int others = 0;
    for (std::size_t i = 0; i < set.size(); ++i)
    {
        const auto& s = set.spec (i);
        if (s.hidden) continue;
        if (classify (s.name).family == Family::Other)
        {
            ++others;
            CHECK (s.name.rfind ("Macro ", 0) == 0);
        }
    }
    CHECK (others == neptune::core::control::kMacros);
    CHECK (classify ("Router Switch 3").family == Family::Router);
    CHECK (classify ("Router Rule 12 Transpose").family == Family::Router);
    CHECK (classify ("Router Switch 3").scope == Scope::Global);
}
