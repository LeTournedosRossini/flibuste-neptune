// AnalogSynth : le synthé de Neptune en plugin indépendant (paramètres, moteur, état, presets, presets livrés).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <set>
#include <vector>

#include "neptune/core/AnalogSynth.h"
#include "neptune/core/Engine.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"
#include "neptune/core/WaveCatalog.h"
#include <nlohmann/json.hpp>

using namespace neptune::core;
using namespace neptune::core::analogsynth;
using Catch::Approx;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48000.0;

struct Render
{
    std::vector<float> l, r;
    float peak() const
    {
        float p = 0.0f;
        for (auto v : l) p = std::max (p, std::fabs (v));
        for (auto v : r) p = std::max (p, std::fabs (v));
        return p;
    }
    bool finite() const
    {
        for (auto v : l) if (! std::isfinite (v)) return false;
        for (auto v : r) if (! std::isfinite (v)) return false;
        return true;
    }
};

// Joue une note puis rend `frames` échantillons par blocs de 128.
Render play (AnalogSynthEngine& e, int key, int frames, bool noteOn = true)
{
    e.prepare (kRate, 128);
    if (noteOn) e.noteOn (0, key, 0.8f);
    Render out;
    for (int done = 0; done < frames; done += 128)
    {
        std::vector<float> l (128), r (128);
        e.process (l.data(), r.data(), 128);
        out.l.insert (out.l.end(), l.begin(), l.end());
        out.r.insert (out.r.end(), r.begin(), r.end());
    }
    return out;
}
} // namespace

TEST_CASE ("analogsynth parameter set is complete and uses neptune track 0 ids", "[analogsynth]")
{
    ParameterSet set;
    std::string error;
    REQUIRE (addParams (set, &error));
    CHECK (set.size() == 2 + 7 + mixer::kSynthKinds + mixer::kSynthEnvKinds);
    for (const auto& k : keyedParams())
        CHECK (set.indexOf (k.id) != ParameterSet::npos);
    CHECK (set.indexOf (mixer::synthParam (0, mixer::kPitchEnvDepth)) != ParameterSet::npos);
    CHECK (set.indexOf (mixer::samplerParam (0, mixer::kFilter2Resonance)) != ParameterSet::npos);
    CHECK (volume == 1000);
    CHECK (pan == 1001);

    std::set<std::string> keys;
    std::set<ParamId> ids;
    for (const auto& k : keyedParams()) { keys.insert (k.key); ids.insert (k.id); }
    CHECK (keys.size() == keyedParams().size());
    CHECK (ids.size() == keyedParams().size());
    CHECK (paramIdForKey ("osc2.detune") == mixer::synthParam (0, mixer::kDetuneOsc2));
    CHECK (paramIdForKey ("nope") == 0);
}

TEST_CASE ("analogsynth plays a note, releases and goes silent", "[analogsynth]")
{
    AnalogSynthEngine e;
    REQUIRE (e.ok());
    auto r = play (e, 69, 4800);
    CHECK (r.finite());
    CHECK (r.peak() > 0.05f);
    CHECK (e.activeVoices() == 1);
    CHECK (e.voiceCount() == 1); // le compteur publié pour l'interface

    e.noteOff (0, 69);
    for (int i = 0; i < 400; ++i)
    {
        std::vector<float> l (128), rr (128);
        e.process (l.data(), rr.data(), 128);
    }
    CHECK (e.activeVoices() == 0);
    CHECK (e.voiceCount() == 0);
}

TEST_CASE ("analogsynth volume and pan apply to the output", "[analogsynth]")
{
    AnalogSynthEngine loud, quiet, left;
    const float peakLoud = play (loud, 60, 9600).peak();
    quiet.params().set (volume, -20.0);
    const float peakQuiet = play (quiet, 60, 9600).peak();
    CHECK (peakQuiet < peakLoud * 0.2f);
    CHECK (peakQuiet > peakLoud * 0.05f);

    left.params().set (pan, -100.0);
    const auto r = play (left, 60, 9600);
    float lp = 0.0f, rp = 0.0f;
    for (std::size_t i = 4800; i < r.l.size(); ++i) { lp = std::max (lp, std::fabs (r.l[i])); rp = std::max (rp, std::fabs (r.r[i])); }
    CHECK (lp > 0.05f);
    CHECK (rp < 1.0e-4f);

    AnalogSynthEngine off;
    off.params().set (volume, -100.0);
    CHECK (play (off, 60, 4800).peak() < 1.0e-6f);
}

TEST_CASE ("analogsynth follows the host modulation without changing the set value", "[analogsynth]")
{
    AnalogSynthEngine plain, modulated;
    const float peakPlain = play (plain, 60, 9600).peak();
    const auto idx = modulated.params().indexOf (volume);
    REQUIRE (idx != ParameterSet::npos);
    modulated.params().setHostModOffsetAt (idx, -0.1f);               // décalage normalisé de la modulation d'hôte
    const float peakMod = play (modulated, 60, 9600).peak();
    CHECK (peakMod < peakPlain * 0.6f);
    CHECK (modulated.params().get (volume) == Approx (plain.params().get (volume)));   // le réglage ne bouge pas
    modulated.params().setHostModOffsetAt (idx, 0.0f);
    CHECK (play (modulated, 60, 9600).peak() == Approx (peakPlain).margin (peakPlain * 0.02f));
}

TEST_CASE ("analogsynth adopts a new patch published from the message thread", "[analogsynth]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    SynthDescription sine;
    sine.osc[0].wave = kWaveSine;
    e.setDescription (sine);
    CHECK (e.description().osc[0].wave == kWaveSine);
    e.noteOn (0, 69, 1.0f);
    std::vector<float> l (4800), r (4800);
    e.process (l.data(), r.data(), 4800);
    // un sinus pur à vélocité 1 : crête bien inférieure à une dent de scie (mêmes marges), signal propre
    float peak = 0.0f;
    for (auto v : l) peak = std::max (peak, std::fabs (v));
    CHECK (peak > 0.02f);
    CHECK (peak < 1.0f);
}

TEST_CASE ("analogsynth ui notes go through the lock-free queue", "[analogsynth]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    e.postNoteOn (64, 0.9f);
    std::vector<float> l (512), r (512);
    e.process (l.data(), r.data(), 512);
    CHECK (e.activeVoices() == 1);
    e.postNoteOff (64);
    e.postPanic();
    e.process (l.data(), r.data(), 512);
    for (int i = 0; i < 100; ++i) e.process (l.data(), r.data(), 512);
    CHECK (e.activeVoices() == 0);
    for (int i = 0; i < 1000; ++i) e.postNoteOn (60 + i % 12, 0.5f); // file pleine : jamais de blocage ni de dépassement
    e.process (l.data(), r.data(), 512);
    CHECK (e.activeVoices() > 0);
}

TEST_CASE ("analogsynth state round trip", "[analogsynth]")
{
    AnalogSynthEngine a;
    a.params().set (mixer::synthParam (0, mixer::kLevelOsc2), 63.0);
    a.params().set (volume, -7.5);
    SynthDescription d;
    d.osc[1].wave = kWaveWarmSaw;
    d.osc[0].unison = 6;
    d.glideMs = 120;
    d.polyphony = PolyphonyMode::Mono;
    const auto text = stateToJson (a.params(), d, "some-preset-id");

    ParameterSet set;
    REQUIRE (addParams (set));
    SynthDescription back;
    std::string presetId;
    REQUIRE (stateFromJson (text, set, back, presetId));
    CHECK (back == sanitised (d));
    CHECK (presetId == "some-preset-id");
    CHECK_THAT (set.get (mixer::synthParam (0, mixer::kLevelOsc2)), WithinAbs (63.0, 1e-9));
    CHECK_THAT (set.get (volume), WithinAbs (-7.5, 1e-9));

    CHECK_FALSE (stateFromJson ("{}", set, back, presetId));
    CHECK_FALSE (stateFromJson ("garbage", set, back, presetId));
    CHECK_FALSE (stateFromJson (R"({"format":"moussaillon-state"})", set, back, presetId));
}

TEST_CASE ("capturing a preset stores captured parameters only", "[analogsynth][preset]")
{
    ParameterSet set;
    REQUIRE (addParams (set));
    set.set (volume, -9.0);
    set.set (pan, 30.0);
    set.set (sampler::filter1Cutoff, 1234.0);
    SynthDescription d;
    d.glideMs = 80;
    const auto cfg = defaultConfig();
    const auto p = capturePreset (set, d, cfg, "Mon son", "Lead");
    CHECK (p.module == moduleId);
    CHECK (p.name == "Mon son");
    CHECK (p.type == "Lead");
    CHECK (p.parameters.count ("filter1.cutoff") == 1);
    CHECK (p.parameters.at ("filter1.cutoff").number == 1234.0);
    CHECK (p.parameters.count ("output.volume") == 0); // libre : jamais dans le preset
    CHECK (p.parameters.count ("output.pan") == 0);
    CHECK_FALSE (p.state.empty());
}

TEST_CASE ("applying a preset leaves free parameters and glide alone", "[analogsynth][preset]")
{
    ParameterSet set;
    REQUIRE (addParams (set));
    SynthDescription source;
    source.osc[0].wave = kWaveSquare;
    source.glideMs = 300;
    set.set (sampler::filter1Cutoff, 900.0);
    const auto cfg = defaultConfig();
    auto p = capturePreset (set, source, cfg, "Cible");

    // l'utilisateur joue autre chose : volume, pan, glissando et coupure changent
    set.set (volume, -12.0);
    set.set (pan, -50.0);
    set.set (sampler::filter1Cutoff, 15000.0);
    SynthDescription current;
    current.glideMs = 10;
    REQUIRE (applyPreset (p, set, current, cfg));
    CHECK_THAT (set.get (sampler::filter1Cutoff), WithinAbs (900.0, 1e-9)); // capturé : restauré
    CHECK_THAT (set.get (volume), WithinAbs (-12.0, 1e-9));                 // libre : inchangé
    CHECK_THAT (set.get (pan), WithinAbs (-50.0, 1e-9));
    CHECK (current.osc[0].wave == kWaveSquare);
    CHECK (current.glideMs == 10);                                          // glissando libre : inchangé

    // exception du preset : le volume devient capturé pour CE preset
    p.captureOverrides["output.volume"] = preset::Capture::Captured;
    p.parameters["output.volume"] = preset::ParamValue::ofNumber (-3.0);
    REQUIRE (applyPreset (p, set, current, cfg));
    CHECK_THAT (set.get (volume), WithinAbs (-3.0, 1e-9));

    // preset d'un autre module : refusé
    p.module = "hammond";
    CHECK_FALSE (applyPreset (p, set, current, cfg));
}

TEST_CASE ("factory presets are valid, stable and audible", "[analogsynth][preset]")
{
    const auto list = factoryPresets();
    REQUIRE (list.size() >= 6);
    std::set<std::string> ids;
    for (const auto& p : list)
    {
        CHECK (p.module == moduleId);
        CHECK_FALSE (p.name.empty());
        CHECK (ids.insert (p.id).second);
        for (const auto& [key, value] : p.parameters) CHECK (paramIdForKey (key) != 0);

        AnalogSynthEngine e;
        SynthDescription d = e.description();
        REQUIRE (applyPreset (p, e.params(), d, defaultConfig()));
        e.setDescription (d);
        const auto r = play (e, 57, 14400);
        INFO (p.name);
        CHECK (r.finite());
        CHECK (r.peak() > 0.01f);
        CHECK (r.peak() < 1.5f);
    }
    // identifiants stables d'un appel à l'autre (les banques les référencent)
    const auto again = factoryPresets();
    for (std::size_t i = 0; i < list.size(); ++i) CHECK (again[i].id == list[i].id);
}

TEST_CASE ("analogsynth default config frees volume, pan and glide", "[analogsynth][preset]")
{
    const auto c = defaultConfig();
    preset::Preset p;
    CHECK (preset::captureOf ("output.volume", p, c) == preset::Capture::Free);
    CHECK (preset::captureOf ("output.pan", p, c) == preset::Capture::Free);
    CHECK (preset::captureOf (glideKey, p, c) == preset::Capture::Free);
    CHECK (preset::captureOf ("filter1.cutoff", p, c) == preset::Capture::Captured);
}

TEST_CASE ("saving over a preset keeps its identity, metadata and capture exceptions", "[analogsynth][preset]")
{
    ParameterSet set;
    REQUIRE (addParams (set));
    const auto cfg = defaultConfig();
    preset::Preset base;
    base.id = "keep-this-id";
    base.name = "Mon pad";
    base.type = "Pad";
    base.rating = 3;
    base.characteristics = { "doux" };
    base.customFields = { { "Concert", "Noel" } };
    base.derivedFrom = "factory-analogsynth-03";
    base.captureOverrides["output.volume"] = preset::Capture::Captured; // exception : le volume fait partie de CE preset

    set.set (volume, -4.0);
    set.set (sampler::filter1Cutoff, 2500.0);
    const auto saved = capturePreset (set, SynthDescription(), cfg, "ignored", "ignored", &base);
    CHECK (saved.id == "keep-this-id");
    CHECK (saved.name == "Mon pad");
    CHECK (saved.rating == 3);
    CHECK (saved.derivedFrom == "factory-analogsynth-03");
    CHECK (saved.parameters.at ("output.volume").number == -4.0); // capturé par exception
    CHECK (saved.parameters.at ("filter1.cutoff").number == 2500.0);
    CHECK (saved.parameters.count ("output.pan") == 0);           // toujours libre
}

TEST_CASE ("analogsynth publishes the held keys for the on-screen keyboard", "[analogsynth]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    e.noteOn (0, 60, 0.8f);
    e.noteOn (0, 72, 0.8f);
    e.noteOn (0, 100, 0.8f);
    CHECK (e.heldKeyWord (0) == (std::uint64_t (1) << 60));
    CHECK (e.heldKeyWord (1) == ((std::uint64_t (1) << (72 - 64)) | (std::uint64_t (1) << (100 - 64))));
    e.noteOff (0, 72);
    CHECK (e.heldKeyWord (1) == (std::uint64_t (1) << (100 - 64)));
    e.panic();
    CHECK (e.heldKeyWord (0) == 0);
    CHECK (e.heldKeyWord (1) == 0);
}

namespace
{
SynthDescription sineWith (int wave)
{
    SynthDescription d;
    d.osc[0].wave = wave;
    d.ampEnv.attack.ms = 0.0;
    d.ampEnv.release.ms = 10.0;
    d.filters.filter[0].on = false;
    return d;
}


float rms (const std::vector<float>& v, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += v[static_cast<std::size_t> (i)] * v[static_cast<std::size_t> (i)];
    return static_cast<float> (std::sqrt (s / (to - from)));
}
} // namespace

TEST_CASE ("per-voice capture: a held note keeps its old patch and values across a preset change", "[analogsynth][capture]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    const auto cfg = defaultConfig();

    // preset A : sinus, niveau 100 ; preset B : carré, niveau 30, accordé une octave plus haut
    preset::Preset a = capturePreset (e.params(), sineWith (kWaveSine), cfg, "A");
    e.params().set (mixer::synthParam (0, mixer::kLevelOsc1), 30.0);
    e.params().set (sampler::tuneSemitones, 12.0);
    preset::Preset b = capturePreset (e.params(), sineWith (kWaveSquare), cfg, "B");
    e.params().resetAll();
    REQUIRE (e.loadPreset (a, cfg));

    std::vector<float> l (128), r (128);
    auto run = [&] (int blocks, std::vector<float>* collect = nullptr)
    {
        for (int i = 0; i < blocks; ++i)
        {
            e.process (l.data(), r.data(), 128);
            if (collect != nullptr) collect->insert (collect->end(), l.begin(), l.end());
        }
    };
    e.noteOn (0, 57, 1.0f); // la 3 = 220 Hz
    run (50);
    CHECK (e.frozenVoices() == 0);

    REQUIRE (e.loadPreset (b, cfg));
    run (2);
    CHECK (e.frozenVoices() == 1); // la note tenue est figée sur le preset A
    e.noteOn (0, 69, 1.0f);        // 440 Hz, preset B (+12 demi-tons : 880 Hz)
    run (50);
    CHECK (e.activeVoices() == 2);

    // relâcher la note figée : elle finit avec le son de A (sinus, jamais carré), puis disparaît
    e.noteOff (0, 57);
    run (20);
    CHECK (e.frozenVoices() == 0);
    CHECK (e.activeVoices() == 1);
    e.panic();
    run (10);
}

TEST_CASE ("per-voice capture: frozen voices ignore later knob moves, new notes follow them", "[analogsynth][capture]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    const auto cfg = defaultConfig();
    const auto p = capturePreset (e.params(), sineWith (kWaveSine), cfg, "A");
    REQUIRE (e.loadPreset (p, cfg));

    std::vector<float> l (128), r (128), all;
    auto run = [&] (int blocks)
    {
        all.clear();
        for (int i = 0; i < blocks; ++i)
        {
            e.process (l.data(), r.data(), 128);
            all.insert (all.end(), l.begin(), l.end());
        }
    };
    e.noteOn (0, 57, 1.0f);
    run (40);
    const float before = rms (all, 128 * 20, 128 * 40);
    REQUIRE (before > 0.01f);

    REQUIRE (e.loadPreset (p, cfg)); // même preset rechargé : nouvelle génération, la note est figée
    run (2);
    CHECK (e.frozenVoices() == 1);
    e.params().set (mixer::synthParam (0, mixer::kLevelOsc1), 0.0); // niveau coupé après le changement
    run (40);
    const float frozen = rms (all, 128 * 20, 128 * 40);
    CHECK (frozen > before * 0.8f); // la note figée sonne toujours à l'ancien niveau

    e.noteOn (0, 69, 1.0f); // nouvelle note : le niveau coupé s'applique
    run (40);
    // seule la note figée sonne (niveau d'avant) : le niveau global n'a pas augmenté
    CHECK (rms (all, 128 * 20, 128 * 40) < before * 1.2f);
}

TEST_CASE ("per-voice capture: the keeper releases old patches once frozen voices are gone", "[analogsynth][capture]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    const auto cfg = defaultConfig();
    const auto p = capturePreset (e.params(), sineWith (kWaveSine), cfg, "A");
    REQUIRE (e.loadPreset (p, cfg));
    std::vector<float> l (128), r (128);
    e.noteOn (0, 60, 1.0f);
    for (int i = 0; i < 20; ++i) e.process (l.data(), r.data(), 128);
    for (int k = 0; k < 5; ++k)
    {
        REQUIRE (e.loadPreset (p, cfg));
        for (int i = 0; i < 4; ++i) e.process (l.data(), r.data(), 128);
    }
    CHECK (e.frozenVoices() == 1);
    e.noteOff (0, 60);
    for (int i = 0; i < 200; ++i) e.process (l.data(), r.data(), 128);
    CHECK (e.frozenVoices() == 0);
    e.collect(); // pas de patch resté épinglé, pas de crash
    REQUIRE (e.loadPreset (p, cfg));
    e.noteOn (0, 64, 1.0f);
    for (int i = 0; i < 20; ++i) e.process (l.data(), r.data(), 128);
    CHECK (e.activeVoices() == 1);
}

TEST_CASE ("presets apply to any Neptune synth track through idForTrack", "[analogsynth][preset]")
{
    // jeu de paramètres de Neptune : 16 pistes, 16 bus
    ParameterSet set;
    REQUIRE (probe::buildPool (set, mixer::numTracks));
    REQUIRE (sampler::addParams (set));
    REQUIRE (mixer::addParams (set, mixer::numTracks, mixer::numBuses));

    CHECK (idForTrack (volume, 0) == volume);
    CHECK (idForTrack (volume, 3) == probe::trackParam (3, probe::kVolume));
    CHECK (idForTrack (sampler::filter2Cutoff, 4) == mixer::samplerParam (4, mixer::kFilter2Cutoff));
    CHECK (idForTrack (mixer::synthParam (0, mixer::kDetuneOsc3), 7) == mixer::synthParam (7, mixer::kDetuneOsc3));
    for (const auto& k : keyedParams())
        for (int t : { 0, 1, 9, 15 }) CHECK (set.indexOf (idForTrack (k.id, t)) != ParameterSet::npos);

    const auto cfg = defaultConfig();
    set.set (mixer::synthParam (2, mixer::kLevelOsc2), 42.0);
    set.set (mixer::samplerParam (2, mixer::kFilter1Cutoff), 1500.0);
    set.set (probe::trackParam (2, probe::kVolume), -9.0);
    SynthDescription d;
    d.osc[1].wave = kWaveTriangle;
    const auto p = capturePreset (set, d, cfg, "Piste 3", "Pad", nullptr, 2);
    CHECK (p.parameters.at ("osc2.level").number == 42.0);
    CHECK (p.parameters.at ("filter1.cutoff").number == 1500.0);
    CHECK (p.parameters.count ("output.volume") == 0); // le fader de la piste est libre

    SynthDescription target;
    REQUIRE (applyPreset (p, set, target, cfg, 6));
    CHECK (set.get (mixer::synthParam (6, mixer::kLevelOsc2)) == 42.0);
    CHECK (set.get (mixer::samplerParam (6, mixer::kFilter1Cutoff)) == 1500.0);
    CHECK (set.get (mixer::synthParam (5, mixer::kLevelOsc2)) == 0.0); // les autres pistes ne bougent pas
    CHECK (target.osc[1].wave == kWaveTriangle);
    CHECK_FALSE (differsFromPreset (p, set, target, cfg, 6));
    set.set (mixer::synthParam (6, mixer::kLevelOsc2), 43.0);
    CHECK (differsFromPreset (p, set, target, cfg, 6));
}

TEST_CASE ("synth envelope parameters follow the description and survive a state round trip", "[analogsynth][envparams]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    SynthDescription d;
    d.ampEnv.attack.ms = 40.0;
    d.ampEnv.decay.ms = 300.0;
    d.ampEnv.sustain = 0.4;
    d.ampEnv.release.ms = 700.0;
    d.filters.filter[1].env.release.ms = 90.0;
    d.glideMs = 120;
    d.filters.balance = 0.25;
    e.setDescription (d, false);

    // les paramètres ont pris les valeurs de la description
    auto value = [&e] (mixer::SynthEnvKind k) { return e.params().get (mixer::synthEnvParam (0, k)); };
    CHECK_THAT (value (mixer::kAmpAttack), WithinAbs (40.0, 1e-6));
    CHECK_THAT (value (mixer::kAmpSustain), WithinAbs (40.0, 1e-6));
    CHECK_THAT (value (mixer::kFilter2Release), WithinAbs (90.0, 1e-6));
    CHECK_THAT (value (mixer::kSynthGlide), WithinAbs (120.0, 1e-6));
    CHECK_THAT (value (mixer::kFilterBalance), WithinAbs (25.0, 1e-6));

    // un changement de paramètre (automation) se retrouve dans la description lue ensuite
    e.params().set (mixer::synthEnvParam (0, mixer::kAmpRelease), 1500.0);
    CHECK_THAT (e.description().ampEnv.release.ms, WithinAbs (1500.0, 1e-6));

    // aller-retour de l'état : la description enregistrée porte la valeur courante
    const auto text = stateToJson (e.params(), e.description(), "");
    ParameterSet set;
    REQUIRE (addParams (set));
    SynthDescription back;
    std::string id;
    REQUIRE (stateFromJson (text, set, back, id));
    CHECK_THAT (back.ampEnv.release.ms, WithinAbs (1500.0, 1e-6));
    CHECK_THAT (set.get (mixer::synthEnvParam (0, mixer::kAmpRelease)), WithinAbs (1500.0, 1e-6));
}

TEST_CASE ("an old state without envelope parameters takes them from its description", "[analogsynth][envparams]")
{
    SynthDescription d;
    d.ampEnv.release.ms = 640.0;
    const auto text = stateToJson (ParameterSet {}, d, ""); // aucun paramètre : comme un état d'avant
    ParameterSet set;
    REQUIRE (addParams (set));
    SynthDescription back;
    std::string id;
    REQUIRE (stateFromJson (text, set, back, id));
    CHECK_THAT (set.get (mixer::synthEnvParam (0, mixer::kAmpRelease)), WithinAbs (640.0, 1e-6));
}

TEST_CASE ("the release time automated on the track is used at note off", "[analogsynth][envparams]")
{
    AnalogSynthEngine e;
    e.prepare (kRate, 128);
    SynthDescription d;
    d.ampEnv.attack.ms = 0.0;
    d.ampEnv.release.ms = 2000.0; // long dans la description
    d.filters.filter[0].on = false;
    e.setDescription (d, false);
    e.params().set (mixer::synthEnvParam (0, mixer::kAmpRelease), 20.0); // court par automation

    std::vector<float> l (128), r (128);
    e.noteOn (0, 60, 1.0f);
    for (int i = 0; i < 20; ++i) e.process (l.data(), r.data(), 128);
    REQUIRE (e.activeVoices() == 1);
    e.noteOff (0, 60);
    for (int i = 0; i < 30; ++i) e.process (l.data(), r.data(), 128); // ~80 ms
    CHECK (e.activeVoices() == 0);
}

TEST_CASE ("state saved, reloaded and saved again is identical even with awkward parameter values", "[analogsynth][envparams]")
{
    AnalogSynthEngine e;
    e.params().set (mixer::synthEnvParam (0, mixer::kSynthGlide), 123.456);       // la description garde des millisecondes entières
    e.params().set (mixer::synthEnvParam (0, mixer::kAmpRelease), 0.2);           // sous le minimum de relâchement de la description
    e.params().set (mixer::synthEnvParam (0, mixer::kFilter1Decay), 777.7777);
    e.params().set (mixer::synthEnvParam (0, mixer::kFilterBalance), 33.3333);
    const auto first = stateToJson (e.params(), e.description(), "");

    AnalogSynthEngine f;
    SynthDescription d;
    std::string id;
    REQUIRE (stateFromJson (first, f.params(), d, id));
    f.setDescription (d, false);
    const auto second = stateToJson (f.params(), f.description(), "");
    CHECK (first == second);
}

TEST_CASE ("analogsynth per-oscillator parameter names carry the right oscillator number", "[analogsynth]")
{
    AnalogSynthEngine e;
    for (int o = 0; o < 3; ++o)
    {
        const auto number = std::string (" ") + std::to_string (o + 1) + " ";
        auto nameOf = [&] (mixer::SynthKind k) { return e.params().spec (e.params().indexOf (mixer::synthParam (0, k))).name; };
        CHECK (nameOf (static_cast<mixer::SynthKind> (mixer::kFineOsc1 + o)) == "Osc" + number + "Fine");
        CHECK (nameOf (static_cast<mixer::SynthKind> (mixer::kPulseOsc1 + o)) == "Osc" + number + "Pulse Width");
        CHECK (nameOf (static_cast<mixer::SynthKind> (mixer::kDetuneOsc1 + o)) == "Osc" + number + "Unison Detune");
    }
}

TEST_CASE ("analogsynth state: the modulation matrix is saved when it is not empty, and an empty one leaves the state unchanged", "[analogsynth][modulation]")
{
    analogsynth::AnalogSynthEngine e;
    REQUIRE (e.ok());
    const auto without = stateToJson (e.params(), e.description(), "");
    modulation::Matrix empty;
    CHECK (stateToJson (e.params(), e.description(), "", &empty) == without);   // sans matrice : identique octet pour octet à l'état d'avant

    modulation::Matrix m;
    m.lfos[0].shape = modulation::LfoShape::Triangle;
    m.lfos[0].rateHz = 5.5;
    modulation::Route r;
    r.source = { modulation::SourceKind::Lfo, 0, 0 };
    r.dest = analogsynth::keyedParams().front().id;
    r.amount = 0.3;
    m.routes.push_back (r);
    const auto with = stateToJson (e.params(), e.description(), "", &m);
    CHECK (with != without);

    ParameterSet set;
    REQUIRE (analogsynth::addParams (set));
    SynthDescription back;
    std::string id;
    modulation::Matrix loaded;
    REQUIRE (stateFromJson (with, set, back, id, &loaded));
    CHECK (loaded == m);
    modulation::Matrix none;
    none.lfos[1].rateHz = 3.0;   // une valeur de départ non vide doit être remise à vide par un état sans matrice
    REQUIRE (stateFromJson (without, set, back, id, &none));
    CHECK (none == modulation::Matrix {});
    CHECK (stateFromJson (without, set, back, id));   // l'appel d'avant, sans matrice, marche toujours
}

TEST_CASE ("preset: the modulation field round trips, is bounded and defaults to empty", "[preset][modulation]")
{
    preset::Preset p;
    p.id = "x";
    p.name = "With modulation";
    p.module = "analogsynth";
    modulation::Matrix m;
    m.lfos[2].rateHz = 7.0;
    p.modulation = modulation::toJson (m);
    bool ok = false;
    const auto back = preset::presetFromJson (preset::toJson (p), &ok);
    REQUIRE (ok);
    CHECK (modulation::fromJson (back.modulation) == m);
    preset::Preset plain;
    plain.id = "y";
    plain.name = "Plain";
    const auto backPlain = preset::presetFromJson (preset::toJson (plain), &ok);
    REQUIRE (ok);
    CHECK (backPlain.modulation.empty());
}

TEST_CASE ("analogsynth pitch bend: the wheel moves every voice by up to the range of the patch, and nothing when it is centred", "[analogsynth][bend]")
{
    // fréquence par passages à zéro montants sur la partie stable du rendu
    auto frequency = [] (const Render& r)
    {
        int crossings = 0;
        double first = -1.0, last = -1.0;
        for (std::size_t i = 4800 + 1; i < r.l.size(); ++i)
            if (r.l[i - 1] < 0.0f && r.l[i] >= 0.0f)
            {
                const double t = static_cast<double> (i - 1) + static_cast<double> (-r.l[i - 1]) / static_cast<double> (r.l[i] - r.l[i - 1]);
                if (first < 0.0) first = t;
                last = t;
                ++crossings;
            }
        return crossings > 2 ? (crossings - 1) * kRate / (last - first) : 0.0;
    };
    auto render = [&] (float bend, int range)
    {
        AnalogSynthEngine e;
        REQUIRE (e.ok());
        auto d = e.description();
        d.osc[0].wave = kWaveSine;   // sinus : une seule traversée par période
        d.filters.filter[0].on = false;
        d.bendRange = range;
        e.setDescription (d);
        e.prepare (kRate, 128);
        e.setPitchBend (bend);
        return play (e, 69, 24000);
    };
    const double centre = frequency (render (0.0f, 2));
    REQUIRE (centre > 400.0);
    CHECK (centre == Approx (440.0).epsilon (0.01));
    CHECK (frequency (render (1.0f, 2)) / centre == Approx (std::pow (2.0, 2.0 / 12.0)).epsilon (0.003));    // +2 demi-tons
    CHECK (frequency (render (-1.0f, 2)) / centre == Approx (std::pow (2.0, -2.0 / 12.0)).epsilon (0.003));  // -2 demi-tons
    CHECK (frequency (render (0.5f, 12)) / centre == Approx (std::pow (2.0, 6.0 / 12.0)).epsilon (0.003));   // la moitié d'une plage de 12
    CHECK (frequency (render (1.0f, 0)) / centre == Approx (1.0).epsilon (0.003));                          // plage 0 : la molette ne change rien
}

TEST_CASE ("synth description: the bend range is bounded and written only when it is not the default", "[synth][bend]")
{
    SynthDescription d;
    CHECK (d.bendRange == 2);
    CHECK (toJson (d).find ("bend_range") == std::string::npos);   // les fichiers d'avant se réécrivent à l'identique
    d.bendRange = 7;
    CHECK (synthFromJson (toJson (d)).bendRange == 7);
    d.bendRange = 99;
    CHECK (sanitised (d).bendRange == SynthDescription::maxBendRange);
    d.bendRange = -4;
    CHECK (sanitised (d).bendRange == 0);
}

TEST_CASE ("neptune engine: synth and FM tracks follow the pitch wheel of the host and of the screen", "[engine][bend]")
{
    auto frequency = [] (const std::vector<float>& v, std::size_t from)
    {
        int crossings = 0;
        double first = -1.0, last = -1.0;
        for (std::size_t i = from + 1; i < v.size(); ++i)
            if (v[i - 1] < 0.0f && v[i] >= 0.0f)
            {
                const double t = static_cast<double> (i - 1) + static_cast<double> (-v[i - 1]) / static_cast<double> (v[i] - v[i - 1]);
                if (first < 0.0) first = t;
                last = t;
                ++crossings;
            }
        return crossings > 2 ? (crossings - 1) * kRate / (last - first) : 0.0;
    };
    auto run = [&] (TrackType type, int midiBend14, float uiBend)
    {
        Engine e;
        e.prepare (kRate, 256);
        e.setTrackActive (0, true);
        e.setTrackType (0, type);
        if (type == TrackType::Synth)
        {
            SynthDescription d;
            d.osc[0].wave = kWaveSine;   // sinus : une seule traversée par période
            d.filters.filter[0].on = false;
            e.publishSynth (0, std::make_shared<SynthPatch> (d));
        }
        else
            e.publishFm (0, std::make_shared<fm::FmDescription>());
        e.pitchBendMidi (0, midiBend14);
        e.setUiPitchBend (0, uiBend);
        std::vector<float> all, l (256), r (256);
        float* out[2] = { l.data(), r.data() };
        e.process (out, 2, 256);
        e.noteOnTrack (0, 0, 69, 0.8f);
        for (int i = 0; i < 100; ++i)
        {
            e.process (out, 2, 256);
            all.insert (all.end(), l.begin(), l.end());
        }
        return frequency (all, 6000);
    };
    for (const auto type : { TrackType::Synth, TrackType::Fm })
    {
        const double centre = run (type, 8192, 0.0f);
        REQUIRE (centre > 100.0);
        INFO ("type " << static_cast<int> (type) << " centre " << centre);
        CHECK (run (type, 16383, 0.0f) / centre == Approx (std::pow (2.0, 2.0 / 12.0)).epsilon (0.01));    // molette de l'hôte en haut : +2 demi-tons
        CHECK (run (type, 0, 0.0f) / centre == Approx (std::pow (2.0, -2.0 / 12.0)).epsilon (0.01));       // en bas : -2
        CHECK (run (type, 16383, 0.5f) / centre == Approx (std::pow (2.0, 1.0 / 12.0)).epsilon (0.01));    // la molette de l'écran prime tant qu'elle n'est pas au repos
    }
}

TEST_CASE ("analogsynth : une clé « carte » dans l'état (réglages de Carte) ne gêne pas la lecture", "[analogsynth][carte]")
{
    AnalogSynthEngine a;
    SynthDescription d;
    d.glideMs = 77;
    auto doc = nlohmann::json::parse (stateToJson (a.params(), d, "p1"));
    doc["carte"] = { { "follow", true }, { "motifs", nlohmann::json::array() } };
    ParameterSet set;
    REQUIRE (addParams (set));
    SynthDescription back;
    std::string id;
    REQUIRE (stateFromJson (doc.dump(), set, back, id));
    CHECK (back == sanitised (d));
    CHECK (id == "p1");
}
