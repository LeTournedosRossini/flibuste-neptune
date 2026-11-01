#include "neptune/core/Parameter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace neptune::core
{
// ---------------------------------------------------------------- ParamSpec

ParamSpec ParamSpec::continuous (ParamId id, std::string name, Unit unit, double min, double max, double def,
                                 Curve curve, double exponent)
{
    ParamSpec s;
    s.id = id; s.name = std::move (name); s.unit = unit; s.kind = Kind::Continuous;
    s.min = min; s.max = max; s.defaultValue = def; s.curve = curve; s.curveExponent = exponent;
    return s;
}

ParamSpec ParamSpec::stepped (ParamId id, std::string name, Unit unit, int min, int max, int def)
{
    ParamSpec s;
    s.id = id; s.name = std::move (name); s.unit = unit; s.kind = Kind::Stepped;
    s.min = min; s.max = max; s.defaultValue = def;
    return s;
}

ParamSpec ParamSpec::toggle (ParamId id, std::string name, bool def)
{
    ParamSpec s;
    s.id = id; s.name = std::move (name); s.kind = Kind::Toggle;
    s.min = 0.0; s.max = 1.0; s.defaultValue = def ? 1.0 : 0.0;
    return s;
}

ParamSpec ParamSpec::choice (ParamId id, std::string name, std::vector<std::string> choices, int def)
{
    ParamSpec s;
    s.id = id; s.name = std::move (name); s.kind = Kind::Choice;
    s.min = 0.0; s.max = std::max<double> (0.0, static_cast<double> (choices.size()) - 1.0);
    s.defaultValue = def; s.choices = std::move (choices);
    return s;
}

std::string ParamSpec::validate() const
{
    if (name.empty()) return "nom vide";
    if (! std::isfinite (min) || ! std::isfinite (max) || ! std::isfinite (defaultValue)) return "valeur non finie";
    if (kind == Kind::Choice)
    {
        if (choices.size() < 2) return "un choix exige au moins 2 entrées";
    }
    else if (! (min < max)) return "min doit être inférieur à max";

    if (defaultValue < min || defaultValue > max) return "défaut hors de [min, max]";
    if (kind != Kind::Continuous && std::floor (defaultValue) != defaultValue) return "défaut non entier";
    if (kind == Kind::Stepped && (std::floor (min) != min || std::floor (max) != max)) return "bornes non entières";

    if (kind == Kind::Continuous)
    {
        if (curve == Curve::Exponential && ! (min > 0.0)) return "courbe exponentielle : min doit être > 0";
        if (curve == Curve::Power && ! (curveExponent > 0.0)) return "courbe puissance : exposant doit être > 0";
    }
    return {};
}

double ParamSpec::clamp (double plain) const noexcept
{
    if (! std::isfinite (plain)) return defaultValue;
    plain = std::min (std::max (plain, min), max);
    if (kind != Kind::Continuous) plain = std::round (plain) + 0.0; // + 0.0 : -0.0 devient +0.0 (texte « -0 st » incohérent)
    return plain;
}

double ParamSpec::toNormalized (double plain) const noexcept
{
    const double v = clamp (plain);
    const double range = max - min;
    if (kind != Kind::Continuous) return (v - min) / range;
    switch (curve)
    {
        case Curve::Exponential: return std::log (v / min) / std::log (max / min);
        case Curve::Power:       return std::pow ((v - min) / range, 1.0 / curveExponent);
        case Curve::Linear:      break;
    }
    return (v - min) / range;
}

double ParamSpec::fromNormalized (double n) const noexcept
{
    n = std::isfinite (n) ? std::min (std::max (n, 0.0), 1.0) : toNormalized (defaultValue);
    double v;
    if (kind != Kind::Continuous) v = min + (max - min) * n;
    else switch (curve)
    {
        case Curve::Exponential: v = min * std::pow (max / min, n); break;
        case Curve::Power:       v = min + (max - min) * std::pow (n, curveExponent); break;
        default:                 v = min + (max - min) * n; break;
    }
    return clamp (v);
}

// ---------------------------------------------------------------- ParameterSet

bool ParameterSet::add (ParamSpec spec, std::string* error)
{
    auto fail = [&] (std::string msg) { if (error) *error = std::move (msg); return false; };

    if (auto err = spec.validate(); ! err.empty()) return fail (std::move (err));
    if (indexOf (spec.id) != npos) return fail ("identifiant déjà utilisé : " + std::to_string (spec.id));

    const ParamId id = spec.id;
    slots_.emplace_back (std::move (spec));
    const auto pos = std::lower_bound (index_.begin(), index_.end(), id,
                                       [] (const auto& e, ParamId v) { return e.first < v; });
    index_.insert (pos, { id, slots_.size() - 1 });
    return true;
}

std::size_t ParameterSet::indexOf (ParamId id) const noexcept
{
    const auto it = std::lower_bound (index_.begin(), index_.end(), id,
                                      [] (const auto& e, ParamId v) { return e.first < v; });
    return (it != index_.end() && it->first == id) ? it->second : npos;
}

const ParamSpec* ParameterSet::find (ParamId id) const noexcept
{
    const auto i = indexOf (id);
    return i == npos ? nullptr : &slots_[i].spec;
}

double ParameterSet::getNormalizedAt (std::size_t index) const noexcept
{
    return slots_[index].spec.toNormalized (getAt (index));
}

bool ParameterSet::setAt (std::size_t index, double plain) noexcept
{
    auto& s = slots_[index];
    const double v = s.spec.clamp (plain);
    const double old = s.value.exchange (v, std::memory_order_relaxed);
    if (old == v) return false;
    s.version.fetch_add (1, std::memory_order_relaxed);
    return true;
}

bool ParameterSet::setNormalizedAt (std::size_t index, double n) noexcept
{
    return setAt (index, slots_[index].spec.fromNormalized (n));
}

double ParameterSet::get (ParamId id) const noexcept
{
    const auto i = indexOf (id);
    return i == npos ? 0.0 : getAt (i);
}

double ParameterSet::getNormalized (ParamId id) const noexcept
{
    const auto i = indexOf (id);
    return i == npos ? 0.0 : getNormalizedAt (i);
}

bool ParameterSet::set (ParamId id, double plain) noexcept
{
    const auto i = indexOf (id);
    return i != npos && setAt (i, plain);
}

bool ParameterSet::setNormalized (ParamId id, double n) noexcept
{
    const auto i = indexOf (id);
    return i != npos && setNormalizedAt (i, n);
}

bool ParameterSet::reset (ParamId id) noexcept
{
    const auto i = indexOf (id);
    return i != npos && setAt (i, slots_[i].spec.defaultValue);
}

void ParameterSet::resetAll() noexcept
{
    for (std::size_t i = 0; i < slots_.size(); ++i) setAt (i, slots_[i].spec.defaultValue);
}

std::uint32_t ParameterSet::version (ParamId id) const noexcept
{
    const auto i = indexOf (id);
    return i == npos ? 0u : versionAt (i);
}

// ---------------------------------------------------------------- texte

namespace
{
const char* unitSuffix (Unit u)
{
    switch (u)
    {
        case Unit::Decibel:      return " dB";
        case Unit::Hertz:        return " Hz";
        case Unit::Milliseconds: return " ms";
        case Unit::Seconds:      return " s";
        case Unit::Percent:      return " %";
        case Unit::Cents:        return " ct";
        case Unit::Semitones:    return " st";
        case Unit::Octaves:      return " oct";
        case Unit::Degrees:      return " °";
        case Unit::Samples:      return " smp";
        case Unit::Ratio:        return ":1";
        case Unit::DecibelPerSecond: return " dB/s";
        case Unit::None:         break;
    }
    return "";
}

int defaultDecimals (const ParamSpec& s)
{
    if (s.decimals >= 0) return s.decimals;
    if (s.kind != Kind::Continuous) return 0;
    switch (s.unit)
    {
        case Unit::Decibel: case Unit::Semitones: case Unit::Ratio: case Unit::Seconds: return 1;
        case Unit::Cents: case Unit::Samples: case Unit::Percent: return 0;
        case Unit::Hertz: case Unit::Milliseconds: return 0;
        default: return 2;
    }
}

std::string lower (std::string_view t)
{
    std::string r (t);
    std::transform (r.begin(), r.end(), r.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return r;
}

std::string trim (std::string s)
{
    const auto notSpace = [] (unsigned char c) { return ! std::isspace (c); };
    s.erase (s.begin(), std::find_if (s.begin(), s.end(), notSpace));
    s.erase (std::find_if (s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}
} // namespace

std::string ParameterSet::toText (const ParamSpec& s, double plain) const
{
    const double v = s.clamp (plain);
    if (s.kind == Kind::Choice) return s.choices[static_cast<std::size_t> (v)];
    if (s.kind == Kind::Toggle) return v >= 0.5 ? "ON" : "OFF";
    if (! s.minText.empty() && v <= s.min) return s.minText;

    char buf[64];
    if (s.unit == Unit::Hertz && s.decimals < 0 && v >= 1000.0)
    {
        std::snprintf (buf, sizeof buf, "%.2f kHz", v / 1000.0);
        return buf;
    }
    std::snprintf (buf, sizeof buf, "%.*f%s", defaultDecimals (s), v, unitSuffix (s.unit));
    std::string out = buf;
    // un petit négatif arrondi à zéro s'affiche « -0.00 » : le signe est retiré (le texte doit se relire en la même valeur)
    if (out.size() > 1 && out[0] == '-')
    {
        std::size_t i = 1;
        while (i < out.size() && (out[i] == '0' || out[i] == '.')) ++i;
        if (i == out.size() || ! (out[i] >= '1' && out[i] <= '9')) out.erase (0, 1);
    }
    return out;
}

std::string ParameterSet::toText (ParamId id) const
{
    const auto i = indexOf (id);
    return i == npos ? std::string() : toText (slots_[i].spec, getAt (i));
}

bool ParameterSet::fromText (ParamId id, std::string_view text, double& plainOut) const
{
    const auto i = indexOf (id);
    if (i == npos) return false;
    const ParamSpec& s = slots_[i].spec;
    const std::string t = lower (trim (std::string (text)));
    if (t.empty()) return false;

    if (s.kind == Kind::Choice)
    {
        for (std::size_t c = 0; c < s.choices.size(); ++c)
            if (lower (s.choices[c]) == t) { plainOut = static_cast<double> (c); return true; }
    }
    if (s.kind == Kind::Toggle)
    {
        if (t == "on" || t == "1" || t == "true")  { plainOut = 1.0; return true; }
        if (t == "off" || t == "0" || t == "false") { plainOut = 0.0; return true; }
        return false;
    }
    if (! s.minText.empty() && t == lower (s.minText)) { plainOut = s.min; return true; }
    if (t == "-inf" && s.unit == Unit::Decibel)         { plainOut = s.min; return true; }

    const char* begin = t.c_str();
    char* end = nullptr;
    double v = std::strtod (begin, &end);
    if (end == begin || ! std::isfinite (v)) return false;

    std::string suffix = trim (end);
    if (s.unit == Unit::Hertz && (suffix == "k" || suffix == "khz")) v *= 1000.0;
    else if (s.unit == Unit::Milliseconds && suffix == "s") v *= 1000.0;
    else if (s.unit == Unit::Seconds && suffix == "ms") v /= 1000.0;
    // Tout autre suffixe (unité déjà attendue) est ignoré.

    plainOut = s.clamp (v);
    return true;
}
} // namespace neptune::core
