// neptune_param_tool : écrit en Markdown la liste des réglages d'une piste de Neptune (identifiant, nom, type, plage, défaut, choix), tels que la Façade les lie.
//   neptune_param_tool            piste 1 (indice 0), plus les macros
// Les identifiants d'une autre piste se déduisent par remapParamTrack ; une Façade faite avec ceux de la piste 1 se recharge sur n'importe quelle piste.
#include <cstdio>
#include <string>

#include "neptune/core/Control.h"
#include "neptune/core/FacadeTransfer.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"
#include "neptune/core/TrackExport.h"

using namespace neptune::core;

namespace
{
const char* unitText (Unit u)
{
    switch (u)
    {
        case Unit::Decibel: return "dB";
        case Unit::Hertz: return "Hz";
        case Unit::Milliseconds: return "ms";
        case Unit::Seconds: return "s";
        case Unit::Percent: return "%";
        case Unit::Cents: return "cents";
        case Unit::Semitones: return "demi-tons";
        case Unit::Octaves: return "octaves";
        case Unit::Degrees: return "degrés";
        case Unit::Samples: return "échantillons";
        case Unit::Ratio: return "rapport";
        case Unit::DecibelPerSecond: return "dB/s";
        default: return "";
    }
}

std::string num (double v)
{
    char buf[48];
    std::snprintf (buf, sizeof buf, "%.6g", v);
    return buf;
}

void row (const ParamSpec& s)
{
    std::string type, range;
    switch (s.kind)
    {
        case Kind::Continuous: type = "continu"; range = num (s.min) + " à " + num (s.max); break;
        case Kind::Stepped: type = "pas"; range = num (s.min) + " à " + num (s.max); break;
        case Kind::Toggle: type = "interrupteur"; range = "0 ou 1"; break;
        case Kind::Choice:
            type = "choix";
            for (std::size_t i = 0; i < s.choices.size(); ++i) range += (i ? " / " : "") + std::to_string (i) + "=" + s.choices[i];
            break;
    }
    if (s.kind != Kind::Choice && *unitText (s.unit) != 0) range += std::string (" ") + unitText (s.unit);
    std::string name = s.name;
    for (auto& c : name) if (c == '|') c = '/';
    std::printf ("| %u | %s | %s | %s | %s |\n", static_cast<unsigned> (s.id), name.c_str(), type.c_str(), range.c_str(), num (s.defaultValue).c_str());
}
} // namespace

int main()
{
    ParameterSet set;
    std::string error;
    const int tracks = 16;
    if (! sampler::addParams (set, &error) || ! probe::buildPool (set, tracks, &error) || ! mixer::addParams (set, tracks, mixer::numBuses, &error) || ! control::addMacroParams (set, &error))
    {
        std::fprintf (stderr, "erreur : %s\n", error.c_str());
        return 1;
    }
    std::printf ("| Identifiant | Nom | Type | Plage | Défaut |\n|---|---|---|---|---|\n");
    for (const ParamId id : trackexport::ownedParams (set, 0))
        if (const auto i = set.indexOf (id); i != ParameterSet::npos && ! set.spec (i).hidden) row (set.spec (i));
    std::printf ("\n### Macros (communes à toutes les pistes)\n\n| Identifiant | Nom | Type | Plage | Défaut |\n|---|---|---|---|---|\n");
    for (std::size_t i = 0; i < set.size(); ++i)
    {
        const auto& s = set.spec (i);
        if (s.name.rfind ("Macro", 0) == 0 && ! s.hidden) row (s);
    }
    return 0;
}
