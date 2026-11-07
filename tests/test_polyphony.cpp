// Polyphonie (palier 1, jalon 6) : limite et vol de voix, même touche, mono, legato et glissando.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <vector>

#include "neptune/core/Engine.h"
#include "neptune/core/InstrumentEdit.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/SamplerVoices.h"
#include "neptune/core/Synth.h"

using namespace neptune::core;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::shared_ptr<const SampleData> constant (float v, std::int64_t frames = 96000)
{
    auto s = std::make_shared<SampleData>();
    s->channels = 1;
    s->frames = frames;
    s->sampleRate = kRate;
    s->pcm32.assign (static_cast<std::size_t> (frames), v);
    return s;
}

std::shared_ptr<const SampleData> sine (double freq, std::int64_t frames = 96000)
{
    auto s = std::make_shared<SampleData>();
    s->channels = 1;
    s->frames = frames;
    s->sampleRate = kRate;
    s->pcm32.resize (static_cast<std::size_t> (frames));
    for (std::int64_t i = 0; i < frames; ++i) s->pcm32[static_cast<std::size_t> (i)] = 0.5f * static_cast<float> (std::sin (2.0 * kPi * freq * static_cast<double> (i) / kRate));
    return s;
}

Zone zoneFor (std::shared_ptr<const SampleData> s, int key, PlayMode mode = PlayMode::OneShot)
{
    Zone z;
    z.sample = std::move (s);
    z.keyLow = z.keyHigh = z.rootNote = key;
    z.mode = mode;
    z.loopStart = 0;
    z.loopEnd = z.sample->frames;
    return z;
}

// Une zone par touche 60..71, d'amplitude 2^-(k-60+1) : la somme des échantillons identifie les touches qui sonnent.
Instrument perKeyInstrument()
{
    Instrument inst;
    for (int k = 60; k < 72; ++k) inst.zones.push_back (zoneFor (constant (std::ldexp (1.0f, -(k - 60 + 1))), k));
    return inst;
}

float amp (int key) { return std::ldexp (1.0f, -(key - 60 + 1)); }

struct Rig
{
    Instrument inst;
    SamplerVoices v;
    explicit Rig (Instrument i) : inst (std::move (i))
    {
        inst.finalize();
        v.prepare (kRate);
        v.setInstrument (&inst);
    }
    // Rend `frames` échantillons et rend le dernier (après les fondus de coupure).
    float settle (int frames = 2048)
    {
        std::vector<float> l (static_cast<std::size_t> (frames), 0.0f), r (static_cast<std::size_t> (frames), 0.0f);
        v.render (l.data(), r.data(), frames, 1.0f, 1.0f, 0.0);
        return l[static_cast<std::size_t> (frames - 1)];
    }
};

double frequencyOf (const std::vector<float>& x, std::size_t from, std::size_t to)
{
    int crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++crossings;
    return static_cast<double> (crossings) * kRate / static_cast<double> (to - from);
}
} // namespace

TEST_CASE ("voice limit is adjustable and the oldest voice is stolen by default")
{
    auto inst = perKeyInstrument();
    inst.maxVoices = 3;
    Rig r (std::move (inst));
    for (int k = 60; k < 64; ++k) r.v.noteOn (0, k, 1.0f); // 4 notes, limite 3 : la 60 (la plus ancienne) est volée
    CHECK (r.v.heldVoices() == 3);
    CHECK_THAT (r.settle(), WithinAbs (amp (61) + amp (62) + amp (63), 1e-5));
    CHECK (r.v.activeVoices() == 3);
}

TEST_CASE ("quietest voice is stolen in quietest mode")
{
    auto inst = perKeyInstrument();
    inst.maxVoices = 3;
    inst.stealMode = VoiceStealMode::Quietest;
    Rig r (std::move (inst));
    r.v.noteOn (0, 60, 1.0f);
    r.v.noteOn (0, 61, 1.0f);  // amplitude de zone 1/4 : au niveau 0.25 x vélocité
    r.v.noteOn (0, 62, 0.1f);  // la plus faible : vélocité 0.1
    r.v.noteOn (0, 63, 1.0f);
    CHECK (r.v.heldVoices() == 3);
    // 62 est partie : 60 + 61 + 63 à vélocité 1
    CHECK_THAT (r.settle(), WithinAbs (amp (60) + amp (61) + amp (63), 1e-5));
}

TEST_CASE ("keep low / keep high steal the extreme voice, and drop a new note that is itself the extreme")
{
    {
        auto inst = perKeyInstrument();
        inst.maxVoices = 2;
        inst.stealMode = VoiceStealMode::KeepLow;
        Rig r (std::move (inst));
        r.v.noteOn (0, 60, 1.0f);
        r.v.noteOn (0, 64, 1.0f);
        r.v.noteOn (0, 62, 1.0f);   // la plus aiguë (64) est volée
        CHECK_THAT (r.settle(), WithinAbs (amp (60) + amp (62), 1e-5));
        r.v.noteOn (0, 70, 1.0f);   // plus aiguë que tout : abandonnée
        CHECK_THAT (r.settle(), WithinAbs (amp (60) + amp (62), 1e-5));
        CHECK (r.v.heldVoices() == 2);
    }
    {
        auto inst = perKeyInstrument();
        inst.maxVoices = 2;
        inst.stealMode = VoiceStealMode::KeepHigh;
        Rig r (std::move (inst));
        r.v.noteOn (0, 66, 1.0f);
        r.v.noteOn (0, 62, 1.0f);
        r.v.noteOn (0, 64, 1.0f);   // la plus grave (62) est volée
        CHECK_THAT (r.settle(), WithinAbs (amp (66) + amp (64), 1e-5));
        r.v.noteOn (0, 60, 1.0f);   // plus grave que tout : abandonnée
        CHECK_THAT (r.settle(), WithinAbs (amp (66) + amp (64), 1e-5));
    }
}

TEST_CASE ("same key: stack by default, cut on request")
{
    {
        Rig r (perKeyInstrument());
        r.v.noteOn (0, 60, 1.0f);
        r.v.noteOn (0, 60, 1.0f);
        CHECK (r.v.activeVoices() == 2);
        CHECK_THAT (r.settle(), WithinAbs (2.0f * amp (60), 1e-5));
    }
    {
        auto inst = perKeyInstrument();
        inst.sameKey = SameKeyMode::Cut;
        Rig r (std::move (inst));
        r.v.noteOn (0, 60, 1.0f);
        r.v.noteOn (0, 60, 1.0f);
        r.v.noteOn (0, 61, 1.0f);  // une autre touche n'est pas touchée
        CHECK_THAT (r.settle(), WithinAbs (amp (60) + amp (61), 1e-5));
        CHECK (r.v.activeVoices() == 2);
    }
}

TEST_CASE ("cut fade length follows the instrument setting")
{
    auto make = [] (int fadeMs)
    {
        auto inst = perKeyInstrument();
        inst.sameKey = SameKeyMode::Cut;
        inst.cutFadeMs = fadeMs;
        return inst;
    };
    for (const int ms : { 3, 100 })
    {
        Rig r (make (ms));
        r.v.noteOn (0, 60, 1.0f);
        r.settle (256);
        r.v.noteOn (0, 60, 1.0f); // coupe la première : elle s'éteint en `ms`
        std::vector<float> l (static_cast<std::size_t> (kRate * 0.2), 0.0f);
        r.v.render (l.data(), nullptr, static_cast<int> (l.size()), 1.0f, 1.0f, 0.0);
        // pendant le fondu la somme dépasse le niveau d'une voix ; elle y revient quand la voix coupée est partie
        const int fadeFrames = static_cast<int> (kRate * ms / 1000.0);
        CHECK (l[static_cast<std::size_t> (fadeFrames / 4)] > amp (60) * 1.05f);
        CHECK_THAT (l[static_cast<std::size_t> (fadeFrames + 400)], WithinAbs (amp (60), 1e-5));
    }
}

TEST_CASE ("mono: one note at a time, returns to the previous held key")
{
    auto inst = perKeyInstrument();
    inst.polyphony = PolyphonyMode::Mono;
    for (auto& z : inst.zones) z.mode = PlayMode::Loop; // pour entendre le relâchement
    Rig r (std::move (inst));
    r.v.noteOn (0, 60, 1.0f);
    r.v.noteOn (0, 64, 1.0f);
    CHECK_THAT (r.settle(), WithinAbs (amp (64), 1e-5)); // seule la dernière sonne
    r.v.noteOff (0, 64);                                  // 60 est toujours tenue : on y revient
    CHECK_THAT (r.settle(), WithinAbs (amp (60), 1e-5));
    r.v.noteOff (0, 60);                                  // plus rien de tenu : relâchement normal
    CHECK (r.settle (96000 / 2) == 0.0f);
    CHECK (r.v.activeVoices() == 0);
}

TEST_CASE ("legato: the voice keeps playing and changes pitch, with optional glide")
{
    auto build = [] (int glideMs)
    {
        Instrument inst;
        inst.zones.push_back (zoneFor (sine (440.0, 192000), 69, PlayMode::Loop)); // La3, racine 69
        inst.zones.back().loopEnd = 192000;
        inst.polyphony = PolyphonyMode::Legato;
        inst.glideMs = glideMs;
        return inst;
    };
    const double ratio = std::exp2 (7.0 / 12.0); // quinte

    {   // sans glissando : la hauteur change tout de suite, une seule voix
        Rig r (build (0));
        std::vector<float> a (4096, 0.0f), b (24000, 0.0f);
        r.v.noteOn (0, 69, 1.0f);
        r.v.render (a.data(), nullptr, 4096, 1.0f, 1.0f, 0.0);
        r.v.noteOn (0, 76, 1.0f); // 69 toujours tenue : legato
        CHECK (r.v.activeVoices() == 1);
        r.v.render (b.data(), nullptr, 24000, 1.0f, 1.0f, 0.0);
        CHECK_THAT (frequencyOf (a, 100, 4000), WithinAbs (440.0, 15.0));
        CHECK_THAT (frequencyOf (b, 2000, 22000), WithinAbs (440.0 * ratio, 15.0));
    }
    {   // glissando de 200 ms : la hauteur monte progressivement
        Rig r (build (200));
        std::vector<float> a (4096, 0.0f), g (48000, 0.0f);
        r.v.noteOn (0, 69, 1.0f);
        r.v.render (a.data(), nullptr, 4096, 1.0f, 1.0f, 0.0);
        r.v.noteOn (0, 76, 1.0f);
        r.v.render (g.data(), nullptr, 48000, 1.0f, 1.0f, 0.0);
        const double early = frequencyOf (g, 200, 2200);              // tout début du glissando
        const double late = frequencyOf (g, 20000, 40000);           // après les 200 ms (9600 échantillons)
        CHECK (early < 440.0 * (1.0 + 0.5 * (ratio - 1.0)));
        CHECK (early >= 430.0);
        CHECK_THAT (late, WithinAbs (440.0 * ratio, 15.0));
    }
    {   // legato : si aucune touche n'est tenue, la note repart normalement
        Rig r (build (0));
        r.v.noteOn (0, 69, 1.0f);
        r.v.noteOff (0, 69);
        r.v.noteOn (0, 76, 1.0f);
        CHECK (r.v.activeVoices() >= 1);
    }
}

TEST_CASE ("polyphony settings are saved, bounded, and part of the undoable settings")
{
    InstrumentDescription d;
    d.polyphony = PolyphonyMode::Legato;
    d.maxVoices = 8;
    d.stealMode = VoiceStealMode::KeepLow;
    d.sameKey = SameKeyMode::Cut;
    d.cutFadeMs = 40;
    d.glideMs = 250;
    const auto back = instrumentFromJson (toJson (d));
    CHECK (back == d);

    const auto def = instrumentFromJson (toJson (InstrumentDescription {}));
    CHECK (def.polyphony == PolyphonyMode::Poly);
    CHECK (def.maxVoices == InstrumentDescription::defaultMaxVoices);
    CHECK (def.maxVoices == 64);
    CHECK (def.cutFadeMs == 3);
    const auto bad = instrumentFromJson (R"({"max_voices": 0, "cut_fade_ms": 99999, "glide_ms": -4, "polyphony": "x", "voice_steal": 5, "zones": []})");
    CHECK (bad.maxVoices == 1);
    CHECK (instrumentFromJson (R"({"max_voices": 100000, "zones": []})").maxVoices == InstrumentDescription::maxVoicesLimit);
    CHECK (InstrumentDescription::maxVoicesLimit == 256);
    CHECK (bad.cutFadeMs == InstrumentDescription::maxCutFadeMs);
    CHECK (bad.glideMs == 0);
    CHECK (bad.polyphony == PolyphonyMode::Poly);
    CHECK (bad.stealMode == VoiceStealMode::Oldest);

    InstrumentDescription current;
    const auto before = current;
    auto get = [&] { return current; };
    auto set = [&] (const InstrumentDescription& x) { current = x; };
    UndoManager um;
    um.perform (std::make_unique<InstrumentEditCommand> (current, d, get, set, "Polyphony"));
    CHECK (current == d);
    um.undo();
    CHECK (current == before);
}

TEST_CASE ("building an instrument carries the polyphony settings")
{
    InstrumentDescription d;
    d.polyphony = PolyphonyMode::Mono;
    d.maxVoices = 5;
    d.stealMode = VoiceStealMode::Quietest;
    d.cutFadeMs = 12;
    d.glideMs = 80;
    const auto inst = buildInstrument (d);
    REQUIRE (inst != nullptr);
    CHECK (inst->polyphony == PolyphonyMode::Mono);
    CHECK (inst->maxVoices == 5);
    CHECK (inst->stealMode == VoiceStealMode::Quietest);
    CHECK (inst->cutFadeMs == 12);
    CHECK (inst->glideMs == 80);
}

TEST_CASE ("limit can be raised to 256 voices; the fade slots absorb a burst of steals")
{
    Instrument inst;
    inst.zones.push_back (zoneFor (constant (0.001f, 480000), 60, PlayMode::Loop));
    inst.maxVoices = 256;
    Rig r (std::move (inst));
    for (int i = 0; i < 256; ++i) r.v.noteOn (0, 60, 1.0f);
    CHECK (r.v.heldVoices() == 256);
    // 60 notes de plus d'un coup : 60 voix volées en fondu en même temps (moins que les emplacements de fondu)
    for (int i = 0; i < 60; ++i) r.v.noteOn (0, 60, 1.0f);
    CHECK (r.v.heldVoices() == 256);
    CHECK (r.v.activeVoices() == 256 + 60);
    r.settle (2048);
    CHECK (r.v.activeVoices() == 256);
}

TEST_CASE ("edits of the file pool and of the replace option survive the undo history")
{
    InstrumentDescription current;
    ZoneDescription z; z.samplePath = "a.wav";
    current.zones.push_back (z);
    const auto before = current;
    auto d = current;
    d.pool = { "/s/a.wav", "/s/b.wav" };
    d.compensateOnReplace = false;
    auto get = [&] { return current; };
    auto set = [&] (const InstrumentDescription& x) { current = x; };
    UndoManager um;
    um.perform (std::make_unique<InstrumentEditCommand> (current, d, get, set, "Pool"));   // même nombre de zones : la différence ne porte que sur les réglages
    CHECK (current.pool == d.pool);
    CHECK_FALSE (current.compensateOnReplace);
    um.undo();
    CHECK (current == before);
    um.redo();
    CHECK (current == d);
}

// ---- limite globale de voix (Engine::enforceGlobalVoiceLimit) : une seule limite pour toutes les pistes ----
namespace
{
struct GlobalRig
{
    Engine engine;
    std::vector<std::vector<float>> storage;
    std::vector<float*> chans;
    int block = 64;
    explicit GlobalRig (int limit)
    {
        engine.prepare (48000.0, block);
        storage.assign (64, std::vector<float> (static_cast<std::size_t> (block), 0.0f));
        for (auto& s : storage) chans.push_back (s.data());
        neptune::core::mixer::MasterSettings m;
        m.maxVoices = limit;
        engine.setMasterSettings (m);
        for (int t = 0; t < 2; ++t)
        {
            engine.setTrackType (t, neptune::core::TrackType::Synth);
            engine.setTrackActive (t, true);
            neptune::core::SynthDescription d;
            d.maxVoices = 16;
            engine.publishSynth (t, std::make_shared<neptune::core::SynthPatch> (d));
        }
        run (4);
    }
    void run (int blocks) { for (int i = 0; i < blocks; ++i) engine.process (chans.data(), static_cast<int> (chans.size()), block); }
    int voices() { return engine.activeVoices (0) + engine.activeVoices (1); }
};
} // namespace

TEST_CASE ("global voice limit: one limit shared by all tracks, the weakest voice goes first")
{
    GlobalRig r (4);
    for (int k = 60; k < 63; ++k) r.engine.noteOnTrack (0, 0, k, 1.0f);   // 3 voix sur la piste 1
    r.engine.noteOnTrack (1, 0, 72, 1.0f);                               // la 4e : sur la piste 2
    r.run (8);
    CHECK (r.voices() == 4);
    r.engine.noteOnTrack (1, 0, 73, 1.0f);   // la 5e dépasse la limite : une voix est sacrifiée, où qu'elle soit
    r.run (200);                             // le temps que la voix volée finisse son fondu
    CHECK (r.voices() == 4);
}

TEST_CASE ("global voice limit: a deactivated track drops its voices and no longer counts against the limit")
{
    GlobalRig r (4);
    for (int k = 60; k < 63; ++k) r.engine.noteOnTrack (0, 0, k, 1.0f);
    r.run (8);
    r.engine.setTrackActive (0, false);   // la piste 1 est désactivée : le moteur arrête ses voix au bloc suivant (donc plus rien à compter ni à sacrifier)
    r.run (4);
    CHECK (r.engine.activeVoices (0) == 0);
    for (int k = 72; k < 76; ++k) r.engine.noteOnTrack (1, 0, k, 1.0f);   // quatre notes sur la piste 2 : exactement la limite, rien n'est volé
    r.run (200);
    CHECK (r.engine.activeVoices (1) == 4);   // aucune n'a été volée à cause des voix de la piste 1
    r.engine.noteOnTrack (1, 0, 76, 1.0f);    // une de plus : une seule voix de la piste 2 est sacrifiée
    r.run (200);
    CHECK (r.engine.activeVoices (1) == 4);
    CHECK (r.engine.activeVoices (0) == 0);
}
