#include "neptune/core/AnalogSynth.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"

namespace neptune::core::analogsynth
{
using nlohmann::json;
namespace
{
constexpr const char* kFormat = "analogsynth-state";
constexpr int kSchema = 1;

const std::vector<KeyedParam>& buildKeys()
{
    static const std::vector<KeyedParam> keys = []
    {
        std::vector<KeyedParam> k;
        k.push_back ({ "output.volume", volume });
        k.push_back ({ "output.pan", pan });
        k.push_back ({ "synth.gain", sampler::gain });
        k.push_back ({ "synth.tune.semitones", sampler::tuneSemitones });
        k.push_back ({ "synth.tune.cents", sampler::tuneCents });
        k.push_back ({ "filter1.cutoff", sampler::filter1Cutoff });
        k.push_back ({ "filter1.resonance", sampler::filter1Resonance });
        k.push_back ({ "filter2.cutoff", sampler::filter2Cutoff });
        k.push_back ({ "filter2.resonance", sampler::filter2Resonance });
        static const char* const names[mixer::kSynthKinds] = {
            "osc1.level", "osc2.level", "osc3.level", "sub.level", "noise.level", "osc1.fine", "osc2.fine", "osc3.fine",
            "osc1.pulse", "osc2.pulse", "osc3.pulse", "osc1.detune", "osc2.detune", "osc3.detune", "pitchenv.depth" };
        for (int i = 0; i < mixer::kSynthKinds; ++i) k.push_back ({ names[i], mixer::synthParam (0, static_cast<mixer::SynthKind> (i)) });
        return k;
    }();
    return keys;
}
} // namespace

const std::vector<KeyedParam>& keyedParams() { return buildKeys(); }

ParamId paramIdForKey (std::string_view key) noexcept
{
    for (const auto& k : buildKeys())
        if (key == k.key) return k.id;
    return 0;
}

bool addParams (ParameterSet& set, std::string* error)
{
    auto vol = ParamSpec::continuous (volume, "Volume", Unit::Decibel, -100.0, 6.0, 0.0);
    vol.minText = "-inf";
    auto gain = ParamSpec::continuous (sampler::gain, "Gain", Unit::Decibel, sampler::silenceDb, 12.0, 0.0);
    gain.minText = "-inf";
    return set.add (std::move (vol), error)
        && set.add (ParamSpec::continuous (pan, "Pan", Unit::Percent, -100.0, 100.0, 0.0), error)
        && set.add (std::move (gain), error)
        && set.add (ParamSpec::stepped (sampler::tuneSemitones, "Tune (semitones)", Unit::Semitones, -24, 24, 0), error)
        && set.add (ParamSpec::continuous (sampler::tuneCents, "Fine tune", Unit::Cents, -100.0, 100.0, 0.0), error)
        && set.add (ParamSpec::continuous (sampler::filter1Cutoff, "Filter 1 Cutoff", Unit::Hertz, 20.0, 20000.0, 20000.0, Curve::Exponential), error)
        && set.add (ParamSpec::continuous (sampler::filter1Resonance, "Filter 1 Reso", Unit::Percent, 0.0, 100.0, 0.0), error)
        && set.add (ParamSpec::continuous (sampler::filter2Cutoff, "Filter 2 Cutoff", Unit::Hertz, 20.0, 20000.0, 20000.0, Curve::Exponential), error)
        && set.add (ParamSpec::continuous (sampler::filter2Resonance, "Filter 2 Reso", Unit::Percent, 0.0, 100.0, 0.0), error)
        && mixer::addSynthParams (set, 0, "", error);
}

preset::LibraryConfig defaultConfig()
{
    preset::LibraryConfig c;
    c.parameterCaptureDefaults["output.volume"] = preset::Capture::Free;
    c.parameterCaptureDefaults["output.pan"] = preset::Capture::Free;
    c.parameterCaptureDefaults[glideKey] = preset::Capture::Free;
    return c;
}

modulation::Matrix defaultMidiMatrix()
{
    using modulation::Route;
    using modulation::SourceKind;
    modulation::Matrix m;
    m.routes.push_back (Route { { SourceKind::ControlChange, 1, 0 }, sampler::filter1Cutoff, 0.5, false });     // molette de modulation
    m.routes.push_back (Route { { SourceKind::ControlChange, 74, 0 }, sampler::filter1Cutoff, 0.5, false });    // brillance
    m.routes.push_back (Route { { SourceKind::ControlChange, 71, 0 }, sampler::filter1Resonance, 0.4, false });  // résonance
    m.routes.push_back (Route { { SourceKind::Aftertouch, 0, 0 }, sampler::filter1Cutoff, 0.3, false });        // pression de canal
    return m;
}

// ---------------------------------------------------------------------------------------------------- enveloppes

namespace
{
EnvelopeDescription& envOf (SynthDescription& d, int e) { return e == 0 ? d.ampEnv : d.filters.filter[static_cast<std::size_t> (e - 1)].env; }
const EnvelopeDescription& envOf (const SynthDescription& d, int e) { return e == 0 ? d.ampEnv : d.filters.filter[static_cast<std::size_t> (e - 1)].env; }
} // namespace

std::vector<std::pair<ParamId, double>> envelopeParamValues (const SynthDescription& d, int track)
{
    std::vector<std::pair<ParamId, double>> out;
    auto id = [track] (int k) { return mixer::synthEnvParam (track, static_cast<mixer::SynthEnvKind> (k)); };
    for (int e = 0; e < 3; ++e)
    {
        const auto& ed = envOf (d, e);
        out.emplace_back (id (e * 4 + 0), ed.attack.ms);
        out.emplace_back (id (e * 4 + 1), ed.decay.ms);
        out.emplace_back (id (e * 4 + 2), ed.sustain * 100.0);
        out.emplace_back (id (e * 4 + 3), ed.release.ms);
    }
    out.emplace_back (id (mixer::kSynthGlide), static_cast<double> (d.glideMs));
    out.emplace_back (id (mixer::kFilterBalance), d.filters.balance * 100.0);
    return out;
}

bool envelopeParamsMatch (const SynthDescription& d, const ParameterSet& set, int track)
{
    return withLiveParams (d, set, track) == sanitised (d);
}

void seedEnvelopeParams (const SynthDescription& d, ParameterSet& set, int track)
{
    if (envelopeParamsMatch (d, set, track)) return;
    for (const auto& [id, v] : envelopeParamValues (d, track)) set.set (id, v);
}

SynthDescription withLiveParams (SynthDescription d, const ParameterSet& set, int track)
{
    auto get = [&set, track] (int k, double fallback)
    {
        const auto i = set.indexOf (mixer::synthEnvParam (track, static_cast<mixer::SynthEnvKind> (k)));
        return i == ParameterSet::npos ? fallback : set.getAt (i);
    };
    for (int e = 0; e < 3; ++e)
    {
        auto& ed = envOf (d, e);
        ed.attack.ms = get (e * 4 + 0, ed.attack.ms);
        ed.decay.ms = get (e * 4 + 1, ed.decay.ms);
        ed.sustain = get (e * 4 + 2, ed.sustain * 100.0) / 100.0;
        ed.release.ms = get (e * 4 + 3, ed.release.ms);
    }
    d.glideMs = static_cast<int> (std::lround (get (mixer::kSynthGlide, d.glideMs)));
    d.filters.balance = get (mixer::kFilterBalance, d.filters.balance * 100.0) / 100.0;
    return sanitised (std::move (d));
}

// ---------------------------------------------------------------------------------------------------- état

std::string stateToJson (const ParameterSet& set, const SynthDescription& d, const std::string& currentPresetId, const modulation::Matrix* modulation)
{
    json doc;
    doc["format"] = kFormat;
    doc["schema_version"] = kSchema;
    json params = json::object();
    for (std::size_t i = 0; i < set.size(); ++i) params[std::to_string (set.spec (i).id)] = set.getAt (i);
    doc["parameters"] = std::move (params);
    const auto synth = json::parse (toJson (d), nullptr, false);
    if (! synth.is_discarded()) doc["synth"] = synth;
    if (! currentPresetId.empty()) doc["preset_id"] = currentPresetId;
    if (modulation != nullptr && *modulation != modulation::Matrix {})
    {
        const auto m = json::parse (modulation::toJson (*modulation), nullptr, false);
        if (! m.is_discarded()) doc["modulation"] = m;
    }
    return doc.dump (2);
}

bool stateFromJson (std::string_view text, ParameterSet& set, SynthDescription& d, std::string& currentPresetId, modulation::Matrix* modulation)
{
    const auto doc = json::parse (text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded() || ! doc.is_object()) return false;
    const auto fmt = doc.find ("format");
    if (fmt == doc.end() || ! fmt->is_string() || fmt->get<std::string>() != kFormat) return false;
    set.resetAll();
    if (const auto p = doc.find ("parameters"); p != doc.end() && p->is_object())
        for (const auto& [key, value] : p->items())
        {
            if (! value.is_number() || ! std::isfinite (value.get<double>())) continue;
            const long long id = std::strtoll (key.c_str(), nullptr, 10);
            if (id > 0) set.set (static_cast<ParamId> (id), value.get<double>());
        }
    d = SynthDescription();
    if (const auto s = doc.find ("synth"); s != doc.end() && s->is_object()) d = synthFromJson (s->dump());
    seedEnvelopeParams (d, set); // la description fait foi (les états d'avant ces paramètres n'en portent pas)
    currentPresetId.clear();
    if (const auto p = doc.find ("preset_id"); p != doc.end() && p->is_string() && p->get<std::string>().size() <= 64) currentPresetId = p->get<std::string>();
    if (modulation != nullptr)
    {
        *modulation = modulation::Matrix {};
        if (const auto m = doc.find ("modulation"); m != doc.end() && m->is_object()) *modulation = modulation::fromJson (m->dump());
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- presets

ParamId idForTrack (ParamId id, int track) noexcept
{
    if (track <= 0) return id;
    if (id >= sampler::gain && id <= sampler::filter2Resonance) // réglages d'instrument : identifiants historiques pour la piste 0
        return mixer::samplerParam (track, static_cast<mixer::SamplerKind> (id - sampler::gain));
    return id + probe::trackStride * static_cast<ParamId> (track); // volume, pan, paramètres du synthé : un bloc par piste
}

preset::Preset capturePreset (const ParameterSet& set, const SynthDescription& d, const preset::LibraryConfig& cfg,
                              const std::string& name, const std::string& type, const preset::Preset* base, int track)
{
    preset::Preset p;
    if (base != nullptr)
    {
        p = *base;
        p.parameters.clear();
        p.factory = false;
    }
    else
    {
        p.id = preset::newId();
        p.name = name;
        p.type = type;
    }
    p.module = moduleId;
    p.state = toJson (d);
    for (const auto& k : keyedParams())
    {
        if (preset::captureOf (k.key, p, cfg) == preset::Capture::Free) continue;
        const auto idx = set.indexOf (idForTrack (k.id, track));
        if (idx != ParameterSet::npos) p.parameters[k.key] = preset::ParamValue::ofNumber (set.getAt (idx));
    }
    return preset::sanitised (std::move (p));
}

SynthDescription presetDescription (const preset::Preset& p, const SynthDescription& current, const preset::LibraryConfig& cfg)
{
    if (p.state.empty()) return current;
    SynthDescription n = synthFromJson (p.state);
    if (preset::captureOf (glideKey, p, cfg) == preset::Capture::Free) n.glideMs = current.glideMs;
    return n;
}

void applyPresetValues (const preset::Preset& p, ParameterSet& set, const preset::LibraryConfig& cfg, int track)
{
    for (const auto& [key, value] : p.parameters)
    {
        const ParamId id = paramIdForKey (key);
        if (id == 0 || value.isText) continue;
        if (preset::captureOf (key, p, cfg) == preset::Capture::Free) continue;
        set.set (idForTrack (id, track), value.number);
    }
}

bool applyPreset (const preset::Preset& p, ParameterSet& set, SynthDescription& d, const preset::LibraryConfig& cfg, int track)
{
    if (! p.module.empty() && p.module != moduleId) return false;
    applyPresetValues (p, set, cfg, track);
    d = presetDescription (p, d, cfg);
    seedEnvelopeParams (d, set, track);
    return true;
}

bool differsFromPreset (const preset::Preset& p, const ParameterSet& set, const SynthDescription& d, const preset::LibraryConfig& cfg,
                        int track)
{
    for (const auto& [key, value] : p.parameters)
    {
        const ParamId id = paramIdForKey (key);
        if (id == 0 || value.isText || preset::captureOf (key, p, cfg) == preset::Capture::Free) continue;
        const auto index = set.indexOf (idForTrack (id, track));
        if (index != ParameterSet::npos && std::fabs (set.getAt (index) - value.number) > 1.0e-9) return true;
    }
    if (! p.state.empty())
    {
        auto stored = synthFromJson (p.state);
        if (preset::captureOf (glideKey, p, cfg) == preset::Capture::Free) stored.glideMs = d.glideMs;
        if (! (stored == d)) return true;
    }
    return false;
}

namespace
{
preset::Preset make (int n, const char* name, const char* type, std::vector<std::string> traits, const SynthDescription& d,
                     std::vector<std::pair<const char*, double>> values)
{
    preset::Preset p;
    p.id = "factory-analogsynth-" + std::string (n < 10 ? "0" : "") + std::to_string (n);
    p.name = name;
    p.type = type;
    p.characteristics = std::move (traits);
    p.module = moduleId;
    p.state = toJson (d);
    for (auto& v : values) p.parameters[v.first] = preset::ParamValue::ofNumber (v.second);
    return p;
}
} // namespace

std::vector<preset::Preset> factoryPresets()
{
    std::vector<preset::Preset> out;

    out.push_back (make (1, "Init", "Lead", { "simple" }, SynthDescription(), {}));

    {
        SynthDescription d;
        d.osc[0].wave = kWaveSaw;
        d.osc[0].unison = 5;
        d.osc[0].curve = DetuneCurve::Supersaw;
        d.osc[0].spread = 0.8;
        d.filters.filter[0].slopeDb = 24;
        d.ampEnv.attack.ms = 5.0;
        d.ampEnv.release.ms = 250.0;
        out.push_back (make (2, "Saw Lead", "Lead", { "brillant", "large" }, d,
                             { { "osc1.detune", 18.0 }, { "filter1.cutoff", 7000.0 }, { "filter1.resonance", 15.0 } }));
    }
    {
        SynthDescription d;
        d.osc[0].wave = kWaveSaw;
        d.osc[0].unison = 4;
        d.osc[0].spread = 0.9;
        d.osc[1].wave = kWaveTriangle;
        d.osc[1].octave = -1;
        d.ampEnv.attack.ms = 700.0;
        d.ampEnv.sustain = 0.85;
        d.ampEnv.release.ms = 1400.0;
        d.filters.filter[0].slopeDb = 12;
        out.push_back (make (3, "Soft Pad", "Pad", { "doux", "large", "lent" }, d,
                             { { "osc1.level", 70.0 }, { "osc2.level", 55.0 }, { "osc1.detune", 14.0 }, { "filter1.cutoff", 1900.0 },
                               { "filter1.resonance", 10.0 } }));
    }
    {
        SynthDescription d;
        d.osc[0].wave = kWaveSaw;
        d.polyphony = PolyphonyMode::Mono;
        d.glideMs = 40;
        d.filters.filter[0].slopeDb = 24;
        d.ampEnv.attack.ms = 3.0;
        d.ampEnv.release.ms = 90.0;
        out.push_back (make (4, "Sub Bass", "Basse", { "gras", "mono" }, d,
                             { { "osc1.level", 65.0 }, { "sub.level", 85.0 }, { "filter1.cutoff", 650.0 }, { "filter1.resonance", 20.0 } }));
    }
    {
        SynthDescription d;
        d.osc[0].wave = kWaveSaw;
        d.ampEnv.attack.ms = 2.0;
        d.ampEnv.decay.ms = 260.0;
        d.ampEnv.sustain = 0.0;
        d.ampEnv.release.ms = 120.0;
        d.filters.filter[0].slopeDb = 24;
        d.filters.filter[0].envDepthOctaves = 3.0;
        d.filters.filter[0].env.attack.ms = 0.0;
        d.filters.filter[0].env.decay.ms = 220.0;
        d.filters.filter[0].env.sustain = 0.0;
        out.push_back (make (5, "Pluck", "Pluck", { "court", "percussif" }, d, { { "filter1.cutoff", 900.0 }, { "filter1.resonance", 18.0 } }));
    }
    {
        SynthDescription d;
        d.osc[0].wave = kWaveOrganFull;
        d.ampEnv.attack.ms = 4.0;
        d.ampEnv.release.ms = 60.0;
        d.filters.filter[0].on = false;
        out.push_back (make (6, "Organ", "Orgue", { "chaud", "tenu" }, d, {}));
    }
    for (auto& p : out) p = preset::sanitised (std::move (p));
    return out;
}

// ---------------------------------------------------------------------------------------------------- moteur

AnalogSynthEngine::AnalogSynthEngine()
{
    std::string error;
    ok_ = addParams (params_, &error);
    auto idx = [this] (ParamId id) { return params_.indexOf (id); };
    for (int k = 0; k < mixer::kSynthKinds; ++k) synthIdx_[static_cast<std::size_t> (k)] = idx (mixer::synthParam (0, static_cast<mixer::SynthKind> (k)));
    for (int k = 0; k < mixer::kSynthEnvKinds; ++k)
        envIdx_[static_cast<std::size_t> (k)] = idx (mixer::synthEnvParam (0, static_cast<mixer::SynthEnvKind> (k)));
    idxVolume_ = idx (volume);
    idxPan_ = idx (pan);
    idxGain_ = idx (sampler::gain);
    idxSemi_ = idx (sampler::tuneSemitones);
    idxCents_ = idx (sampler::tuneCents);
    idxCutoff_ = { idx (sampler::filter1Cutoff), idx (sampler::filter2Cutoff) };
    idxReso_ = { idx (sampler::filter1Resonance), idx (sampler::filter2Resonance) };
    setDescription (SynthDescription());
}

void AnalogSynthEngine::prepare (double sampleRate, int)
{
    voices_.prepare (sampleRate);
    lastFader_ = static_cast<float> (sampler::dbToGain (params_.effectiveAt (idxVolume_)));
    lastGain_ = static_cast<float> (sampler::dbToGain (params_.effectiveAt (idxGain_)));
    float l = 1.0f, r = 1.0f;
    mixer::panGains (mixer::PanMode::Balance, static_cast<float> (params_.effectiveAt (idxPan_) / 100.0), l, r);
    lastPanL_ = l;
    lastPanR_ = r;
}

void AnalogSynthEngine::setDescription (const SynthDescription& d, bool newPreset)
{
    description_ = sanitised (d);
    auto patch = std::make_shared<SynthPatch> (description_);
    patch->freezeRunningVoices = newPreset;
    keeper_.add (patch);
    patchSlot_.publish (std::move (patch));
    patchSlot_.collect();
    pruneKeeper();
    // après la publication (capture par voix : les notes figées gardent les valeurs d'avant) : les paramètres suivent la description
    seedEnvelopeParams (description_, params_);
}

bool AnalogSynthEngine::loadPreset (const preset::Preset& p, const preset::LibraryConfig& cfg)
{
    if (! p.module.empty() && p.module != moduleId) return false;
    // le patch d'abord : les valeurs ne bougent qu'après la publication
    setDescription (presetDescription (p, description_, cfg), true);
    applyPresetValues (p, params_, cfg);
    return true;
}

void AnalogSynthEngine::pruneKeeper()
{
    const auto adopted = voices_.adoptedGeneration(); // l'audio écrit les voix figées avant le patch adopté : lecture dans l'ordre inverse
    const auto oldest = voices_.oldestFrozenGeneration();
    keeper_.prune (oldest, adopted);
}

void AnalogSynthEngine::post (Event::Type t, int key, float velocity) noexcept
{
    const auto head = head_.load (std::memory_order_relaxed);
    if (head - tail_.load (std::memory_order_acquire) >= kQueue) return; // file pleine : l'événement est perdu
    queue_[head % kQueue] = Event { t, key, velocity };
    head_.store (head + 1, std::memory_order_release);
}

void AnalogSynthEngine::drain() noexcept
{
    auto tail = tail_.load (std::memory_order_relaxed);
    const auto head = head_.load (std::memory_order_acquire);
    while (tail != head)
    {
        const Event e = queue_[tail % kQueue];
        switch (e.type)
        {
            case Event::NoteOn: noteOn (0, e.key, e.velocity); break;
            case Event::NoteOff: noteOff (0, e.key); break;
            case Event::Panic: panic(); break;
            case Event::ReleaseAll: releaseAll(); break;
        }
        ++tail;
    }
    tail_.store (tail, std::memory_order_release);
}

void AnalogSynthEngine::process (float* left, float* right, int n) noexcept
{
    if (left == nullptr || n <= 0) return;
    adoptPatch();
    drain();

    std::fill (left, left + n, 0.0f);
    if (right != nullptr) std::fill (right, right + n, 0.0f);

    // le son lit les valeurs effectives : la modulation de l'hôte (et celle de la matrice) s'y ajoute sans changer les réglages
    SynthVoices::Params sp;
    for (int f = 0; f < 2; ++f)
    {
        sp.cutoffHz[f] = params_.effectiveAt (idxCutoff_[static_cast<std::size_t> (f)]);
        sp.resonance[f] = params_.effectiveAt (idxReso_[static_cast<std::size_t> (f)]) / 100.0;
    }
    sp.tuneCents = params_.effectiveAt (idxSemi_) * 100.0 + params_.effectiveAt (idxCents_);
    sp.bend = static_cast<double> (bend_);
    auto get = [this] (mixer::SynthKind k) { return params_.effectiveAt (synthIdx_[static_cast<std::size_t> (k)]); };
    for (int o = 0; o < numSources; ++o) sp.level[o] = get (static_cast<mixer::SynthKind> (mixer::kLevelOsc1 + o));
    for (int o = 0; o < numOscillators; ++o)
    {
        sp.fineCents[o] = get (static_cast<mixer::SynthKind> (mixer::kFineOsc1 + o));
        sp.pulse[o] = get (static_cast<mixer::SynthKind> (mixer::kPulseOsc1 + o));
        sp.unisonDetune[o] = get (static_cast<mixer::SynthKind> (mixer::kDetuneOsc1 + o));
    }
    sp.pitchEnvDepth = get (mixer::kPitchEnvDepth);
    auto env = [this] (mixer::SynthEnvKind k) { return params_.effectiveAt (envIdx_[static_cast<std::size_t> (k)]); };
    auto fill = [&env] (SynthVoices::Params::EnvParams& e, int base)
    {
        e.attackMs = env (static_cast<mixer::SynthEnvKind> (base));
        e.decayMs = env (static_cast<mixer::SynthEnvKind> (base + 1));
        e.sustain = env (static_cast<mixer::SynthEnvKind> (base + 2)) / 100.0;
        e.releaseMs = env (static_cast<mixer::SynthEnvKind> (base + 3));
    };
    fill (sp.ampEnv, 0);
    fill (sp.filterEnv[0], 4);
    fill (sp.filterEnv[1], 8);
    sp.glideMs = env (mixer::kSynthGlide);
    sp.filterBalance = env (mixer::kFilterBalance) / 100.0;

    const float gain = static_cast<float> (sampler::dbToGain (params_.effectiveAt (idxGain_)));
    voices_.render (left, right, n, lastGain_, gain, sp);
    lastGain_ = gain;

    // volume et pan de sortie : rampes linéaires sur le bloc (pas de clic à l'automation)
    const float fader = static_cast<float> (sampler::dbToGain (params_.effectiveAt (idxVolume_)));
    float pl = 1.0f, pr = 1.0f;
    mixer::panGains (mixer::PanMode::Balance, static_cast<float> (params_.effectiveAt (idxPan_) / 100.0), pl, pr);
    const float inv = 1.0f / static_cast<float> (n);
    const float fStep = (fader - lastFader_) * inv, lStep = (pl - lastPanL_) * inv, rStep = (pr - lastPanR_) * inv;
    float f = lastFader_, gl = lastPanL_, gr = lastPanR_, peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        f += fStep; gl += lStep; gr += rStep;
        left[i] *= f * gl;
        peakL = std::max (peakL, std::fabs (left[i]));
        if (right != nullptr)
        {
            right[i] *= f * gr;
            peakR = std::max (peakR, std::fabs (right[i]));
        }
    }
    lastFader_ = fader;
    lastPanL_ = pl;
    lastPanR_ = pr;
    voiceCount_.store (voices_.activeVoices(), std::memory_order_relaxed);
    peakL_.store (peakL, std::memory_order_relaxed);
    peakR_.store (peakR, std::memory_order_relaxed);
}
} // namespace neptune::core::analogsynth
