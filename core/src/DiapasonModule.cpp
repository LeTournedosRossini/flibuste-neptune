#include "neptune/core/DiapasonModule.h"

namespace neptune::core
{
DiapasonModule::DiapasonModule() = default;

bool DiapasonModule::declareParams (ParameterSet& set, std::string* error) const { return generator::addParams (set, error); }

void DiapasonModule::bind (const ParameterSet& set)
{
    map_.clear();
    const auto& own = engine_.params();
    for (std::size_t i = 0; i < set.size(); ++i)
    {
        const auto id = set.spec (i).id;
        if (id < stock::firstModuleParam) continue;
        const auto j = own.indexOf (id);
        if (j != ParameterSet::npos) map_.emplace_back (i, j);
    }
}

void DiapasonModule::prepare (double sampleRate, int maxBlockSize) { engine_.prepare (sampleRate, maxBlockSize); }

void DiapasonModule::reset() noexcept { engine_.reset(); }

void DiapasonModule::process (float* left, float* right, int n, const ParameterSet& params) noexcept
{
    auto& own = engine_.params();
    for (const auto& [from, to] : map_) own.setAt (to, params.getAt (from));
    engine_.process (left, right, n, generator::Transport {});
}
} // namespace neptune::core
