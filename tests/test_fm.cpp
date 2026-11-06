// Module FM de type DX7 : patch et sysex, topologie des algorithmes, moteur msfa, voix (polyphonie, mono / legato, unisson, macros).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>
#include <vector>

#include "neptune/core/FmEngine.h"
#include "neptune/core/FmPatch.h"
#include "neptune/core/FmVoices.h"
#include <nlohmann/json.hpp>

using namespace neptune::core;
using namespace neptune::core::fm;

namespace
{
constexpr double kRate = 48000.0;

struct Rendered
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

struct Rig
{
    FmDescription desc;
    FmVoices voices;
    FmLive live;
    double rate;

    explicit Rig (double sampleRate = kRate) : rate (sampleRate)
    {
        initTables (sampleRate);
        voices.reserve();
        voices.prepare (sampleRate);
    }
    void apply() { voices.setDescription (&desc); }
    Rendered render (int frames)
    {
        Rendered out;
        out.l.assign (static_cast<std::size_t> (frames), 0.0f);
        out.r.assign (static_cast<std::size_t> (frames), 0.0f);
        constexpr int chunk = 128;
        for (int i = 0; i < frames; i += chunk)
        {
            const int n = std::min (chunk, frames - i);
            voices.render (out.l.data() + i, out.r.data() + i, n, 1.0f, 1.0f, live);
        }
        return out;
    }
};

double zeroCrossHz (const std::vector<float>& x, int from, int to, double rate)
{
    int n = 0;
    for (int i = from + 1; i < to; ++i) n += (x[static_cast<std::size_t> (i - 1)] < 0.0f) != (x[static_cast<std::size_t> (i)] < 0.0f) ? 1 : 0;
    return 0.5 * n * rate / (to - from);
}

float rms (const std::vector<float>& v, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += static_cast<double> (v[static_cast<std::size_t> (i)]) * v[static_cast<std::size_t> (i)];
    return static_cast<float> (std::sqrt (s / (to - from)));
}
} // namespace

TEST_CASE ("fm patch: init voice and bounds", "[fm][patch]")
{
    const auto p = Patch::initVoice();
    CHECK (p.name() == "INIT VOICE");
    CHECK (p.algorithm() == 0);
    CHECK (p.op (5, kOutputLevel) == 99); // OP1 est le dernier opérateur du sysex
    CHECK (p.op (0, kOutputLevel) == 0);
    CHECK (p.operatorOn (3));
    CHECK (p.global (kTransposeOffset) == 24);

    Patch q = p;
    q.setOp (2, kOutputLevel, 500);
    CHECK (q.op (2, kOutputLevel) == 99);
    q.setGlobal (kAlgorithmOffset, 80);
    CHECK (q.algorithm() == 31);
    q.setName ("abc\x01xyz");
    CHECK (q.name().size() <= 10);
    q.data[10] = 250;
    q.data[kFeedbackOffset] = 77;
    q.sanitise();
    CHECK (q.data[10] <= 99);
    CHECK (q.feedback() == 7);
}

TEST_CASE ("fm patch: pack and unpack are inverse", "[fm][patch]")
{
    std::mt19937 rng (1234);
    for (int t = 0; t < 50; ++t)
    {
        Patch p = Patch::initVoice();
        for (int op = 0; op < kOperators; ++op)
            for (int f = 0; f < kNumOpFields; ++f)
                p.setOp (op, f, static_cast<int> (rng() % (static_cast<unsigned> (operatorFieldMax (f)) + 1u)));
        for (int i = 126; i <= 144; ++i)
            if (globalByteMax (i) >= 0) p.setGlobal (i, static_cast<int> (rng() % (static_cast<unsigned> (globalByteMax (i)) + 1u)));
        p.setName ("PATCH " + std::to_string (t));
        const auto packed = packVoice (p);
        for (auto b : packed) REQUIRE (b < 0x80);
        const auto back = unpackVoice (packed.data());
        REQUIRE (back == p);
    }
}

TEST_CASE ("fm sysex: single voice and bulk round trips, checksum, junk", "[fm][sysex]")
{
    Patch p = Patch::initVoice();
    p.setGlobal (kAlgorithmOffset, 21);
    p.setOp (1, kOutputLevel, 73);
    p.setName ("ROUNDTRIP");

    const auto single = makeSingleVoiceSysex (p, 3);
    REQUIRE (single.size() == 163);
    CHECK (single.front() == 0xF0);
    CHECK (single.back() == 0xF7);
    auto r = parseSysex (single.data(), single.size());
    REQUIRE (r.patches.size() == 1);
    CHECK (r.checksumOk);
    CHECK (r.patches[0] == p);

    std::vector<Patch> bank (32, Patch::initVoice());
    bank[7] = p;
    const auto bulk = makeBulkSysex (bank);
    REQUIRE (bulk.size() == 4104);
    r = parseSysex (bulk.data(), bulk.size());
    REQUIRE (r.patches.size() == 32);
    CHECK (r.checksumOk);
    CHECK (r.patches[7] == p);
    CHECK (r.patches[0].name() == "INIT VOICE");

    // plusieurs messages et octets parasites dans un même fichier
    std::vector<std::uint8_t> file = { 0x00, 0x12 };
    file.insert (file.end(), single.begin(), single.end());
    file.push_back (0x55);
    file.insert (file.end(), single.begin(), single.end());
    CHECK (parseSysex (file.data(), file.size()).patches.size() == 2);

    // somme de contrôle fausse : signalée, le patch est quand même lu
    auto bad = single;
    bad[bad.size() - 2] ^= 0x15;
    r = parseSysex (bad.data(), bad.size());
    CHECK_FALSE (r.checksumOk);
    CHECK (r.patches.size() == 1);

    // n'importe quoi : aucune voix, aucune exception
    std::vector<std::uint8_t> junk (500);
    for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::uint8_t> (i * 7);
    CHECK (parseSysex (junk.data(), junk.size()).patches.empty());
    const std::vector<std::uint8_t> cut = { 0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B, 1, 2, 3 };
    CHECK (parseSysex (cut.data(), cut.size()).patches.empty());
}

TEST_CASE ("fm algorithms: carriers and routing match the DX7", "[fm][algorithm]")
{
    // algorithme 1 : porteuses OP1 et OP3 (indices 5 et 3) ; OP2 module OP1, OP4 -> OP3 -> ... avec feedback sur OP6
    const auto& a1 = algorithmGraph (0);
    CHECK (a1.carrierMask == ((1 << 5) | (1 << 3)));
    CHECK_FALSE (a1.edges.empty());
    CHECK (a1.feedbackFrom >= 0);

    // algorithme 32 : six porteuses, aucune modulation, feedback sur OP6
    const auto& a32 = algorithmGraph (31);
    CHECK (a32.carrierMask == 0x3f);
    CHECK (a32.edges.empty());

    // algorithme 16 et 17 : une seule porteuse (OP1)
    CHECK (algorithmGraph (15).carrierMask == (1 << 5));
    CHECK (algorithmGraph (16).carrierMask == (1 << 5));

    for (int a = 0; a < kNumAlgorithms; ++a)
    {
        const auto& g = algorithmGraph (a);
        CHECK (g.carrierMask != 0);            // toujours au moins une porteuse
        for (const auto& e : g.edges)
        {
            CHECK (e.from >= 0);
            CHECK (e.from < kOperators);
            CHECK (e.to >= 0);
            CHECK (e.to < kOperators);
        }
    }
}

TEST_CASE ("fm engine: an init voice plays a pure tone at the right pitch", "[fm][engine]")
{
    Rig rig;
    rig.apply();
    rig.voices.noteOn (0, 69, 1.0f); // la 440
    const auto out = rig.render (48000);
    REQUIRE (out.finite());
    CHECK (out.peak() > 0.01f);
    CHECK (out.peak() < 1.5f);
    CHECK (std::fabs (zeroCrossHz (out.l, 8000, 40000, kRate) - 440.0) < 4.0);
    // transposition du patch : +12 demi-tons
    Rig up;
    up.desc.patch.setGlobal (kTransposeOffset, 36);
    up.apply();
    up.voices.noteOn (0, 69, 1.0f);
    const auto o2 = up.render (48000);
    CHECK (std::fabs (zeroCrossHz (o2.l, 8000, 40000, kRate) - 880.0) < 8.0);
}

TEST_CASE ("fm engine: release ends the note and frees the voice", "[fm][engine]")
{
    Rig rig;
    rig.desc.patch.setOp (5, kRate4, 80); // relâchement assez rapide
    rig.apply();
    rig.voices.noteOn (0, 60, 0.8f);
    rig.render (4800);
    CHECK (rig.voices.activeVoices() == 1);
    rig.voices.noteOff (0, 60);
    rig.render (48000 * 2);
    CHECK (rig.voices.activeVoices() == 0);
}

TEST_CASE ("fm voices: poly, mono and legato", "[fm][voices]")
{
    {
        Rig rig;
        rig.desc.maxVoices = 4;
        rig.apply();
        for (int k : { 60, 64, 67 }) rig.voices.noteOn (0, k, 0.7f);
        rig.render (256);
        CHECK (rig.voices.activeVoices() == 3);
        // limite : une 5e note vole la plus ancienne
        rig.voices.noteOn (0, 72, 0.7f);
        rig.voices.noteOn (0, 74, 0.7f);
        rig.render (48000 / 50);
        CHECK (rig.voices.heldVoices() <= 4);
    }
    {
        Rig rig;
        rig.desc.polyphony = PolyphonyMode::Mono;
        rig.apply();
        rig.voices.noteOn (0, 60, 0.7f);
        rig.render (512);
        rig.voices.noteOn (0, 64, 0.7f);
        rig.render (512);
        CHECK (rig.voices.heldVoices() == 1);
        rig.voices.noteOff (0, 64); // 60 est encore tenue : elle reprend la main
        rig.render (4800);
        CHECK (rig.voices.heldVoices() == 1);
        rig.voices.noteOff (0, 60);
        rig.render (48000 * 2);
        CHECK (rig.voices.activeVoices() == 0);
    }
    {
        // legato avec glissando : la hauteur monte progressivement vers la nouvelle touche, sans retrigger
        Rig rig;
        rig.desc.polyphony = PolyphonyMode::Legato;
        rig.desc.glideMs = 300;
        rig.apply();
        rig.voices.noteOn (0, 57, 0.8f); // 220 Hz
        rig.render (24000);
        rig.voices.noteOn (0, 69, 0.8f); // une octave plus haut : 440 Hz
        const auto early = rig.render (4800);
        const auto late = rig.render (48000);
        const double fEarly = zeroCrossHz (early.l, 0, 4800, kRate);
        const double fLate = zeroCrossHz (late.l, 36000, 48000, kRate);
        CHECK (fEarly > 215.0);
        CHECK (fEarly < 400.0);  // en route
        CHECK (std::fabs (fLate - 440.0) < 6.0);
    }
}

TEST_CASE ("fm voices: unison widens the stereo image and keeps a bounded level", "[fm][voices]")
{
    Rig mono;
    mono.apply();
    mono.voices.noteOn (0, 60, 0.8f);
    const auto a = mono.render (24000);
    // un seul « voix » : gauche = droite
    double diff = 0.0;
    for (std::size_t i = 4000; i < a.l.size(); ++i) diff += std::fabs (a.l[i] - a.r[i]);
    CHECK (diff < 1.0e-3);

    Rig uni;
    uni.desc.unisonVoices = 5;
    uni.live.unisonDetuneCents = 25.0f;
    uni.live.unisonSpread = 1.0f;
    uni.apply();
    uni.voices.noteOn (0, 60, 0.8f);
    const auto b = uni.render (24000);
    REQUIRE (b.finite());
    diff = 0.0;
    for (std::size_t i = 4000; i < b.l.size(); ++i) diff += std::fabs (b.l[i] - b.r[i]);
    CHECK (diff > 1.0);
    CHECK (b.peak() < a.peak() * 4.0f);
}

TEST_CASE ("fm macros: brightness changes the modulators, op level offsets the carrier", "[fm][macro]")
{
    // algorithme 5 (indice 4) : paires modulateur -> porteuse ; OP2 module OP1
    auto make = [] (float brightness, float opLevel)
    {
        auto rig = std::make_unique<Rig>();
        rig->desc.patch.setGlobal (kAlgorithmOffset, 4);
        for (int op = 0; op < kOperators; ++op) rig->desc.patch.setOp (op, kOutputLevel, 0);
        rig->desc.patch.setOp (5, kOutputLevel, 99); // OP1 porteuse
        rig->desc.patch.setOp (4, kOutputLevel, 60); // OP2 modulateur
        rig->live.brightness = brightness;
        rig->live.opLevel[5] = opLevel;
        rig->apply();
        rig->voices.noteOn (0, 60, 1.0f);
        return rig;
    };
    auto dark = make (-1.0f, 0.0f), bright = make (1.0f, 0.0f);
    const auto d = dark->render (24000), b = bright->render (24000);
    REQUIRE (d.finite());
    REQUIRE (b.finite());
    // plus de modulation : le signal diffère nettement d'un sinus pur
    double diff = 0.0;
    for (std::size_t i = 6000; i < d.l.size(); ++i) diff += std::fabs (d.l[i] - b.l[i]);
    CHECK (diff > 5.0);

    auto quiet = make (0.0f, -40.0f), normal = make (0.0f, 0.0f);
    const auto q = quiet->render (24000), n = normal->render (24000);
    CHECK (rms (q.l, 6000, 24000) < rms (n.l, 6000, 24000) * 0.5f); // -40 pas = -30 dB sur la porteuse
}

TEST_CASE ("fm envelopes last the same real time at 44.1 and 48 kHz", "[fm][engine]")
{
    auto releaseSeconds = [] (double rate)
    {
        Rig rig (rate);
        rig.desc.patch.setOp (5, kRate4, 40); // relâchement long
        rig.apply();
        rig.voices.noteOn (0, 60, 1.0f);
        {
            std::vector<float> l (4800), r (4800);
            rig.voices.render (l.data(), r.data(), 4800, 1.0f, 1.0f, rig.live);
        }
        rig.voices.noteOff (0, 60);
        std::vector<float> l (512), r (512);
        int frames = 0;
        while (rig.voices.activeVoices() > 0 && frames < static_cast<int> (rate * 40.0))
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            rig.voices.render (l.data(), r.data(), 512, 1.0f, 1.0f, rig.live);
            frames += 512;
        }
        return static_cast<double> (frames) / rate;
    };
    const double a = releaseSeconds (44100.0), b = releaseSeconds (48000.0);
    initTables (kRate);
    CHECK (a > 0.2);
    CHECK (std::fabs (a - b) / a < 0.08);
}

TEST_CASE ("fm description json round trip and tolerance", "[fm][patch]")
{
    FmDescription d;
    d.patch.setGlobal (kAlgorithmOffset, 12);
    d.patch.setName ("MY SOUND");
    d.masterTuneCents = -33;
    d.polyphony = PolyphonyMode::Legato;
    d.maxVoices = 9;
    d.stealMode = VoiceStealMode::KeepHigh;
    d.glideMs = 250;
    d.pitchBendRange = 7;
    d.wheelPitchSens = 5;
    d.wheelAmpSens = 11;
    d.unisonVoices = 6;
    CHECK (fmFromJson (toJson (d)) == sanitised (d));
    CHECK (fmFromJson ("garbage") == FmDescription());
    const auto wild = fmFromJson (R"({"max_voices":9999,"unison_voices":-4,"polyphony":"bogus","patch":[1,2]})");
    CHECK (wild.maxVoices == kMaxVoices);
    CHECK (wild.unisonVoices == 1);
}

TEST_CASE ("fm calibration: peak levels of a single carrier and of six carriers", "[fm][engine]")
{
    Rig one;
    one.apply();
    one.voices.noteOn (0, 69, 1.0f);
    const float p1 = one.render (24000).peak();
    Rig six;
    six.desc.patch.setGlobal (kAlgorithmOffset, 31);
    for (int op = 0; op < kOperators; ++op) six.desc.patch.setOp (op, kOutputLevel, 99);
    six.apply();
    six.voices.noteOn (0, 69, 1.0f);
    const float p6 = six.render (24000).peak();
    // niveau de référence de msfa : une porteuse à 99 donne 1/8 de la pleine échelle (marge pour 8 porteuses, comme le DX7)
    CHECK (p1 > 0.10f);
    CHECK (p1 < 0.15f);
    CHECK (p6 > 0.6f);
    CHECK (p6 < 0.9f);
}

#include "neptune/core/Engine.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/MixerState.h"
#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"

TEST_CASE ("fm track in the engine: plays, obeys the master voice limit, state round trip", "[fm][engine]")
{
    Engine e;
    e.prepare (kRate, 256);
    e.setTrackActive (0, true);
    e.setTrackType (0, TrackType::Fm);
    auto d = std::make_shared<FmDescription>();
    e.publishFm (0, d);
    std::vector<float> l (256), r (256);
    float* out[2] = { l.data(), r.data() };
    e.process (out, 2, 256);
    e.noteOnTrack (0, 0, 60, 0.8f);
    float peak = 0.0f;
    for (int i = 0; i < 40; ++i)
    {
        e.process (out, 2, 256);
        for (float v : l) peak = std::max (peak, std::fabs (v));
    }
    CHECK (peak > 0.01f);
    CHECK (e.activeVoices (0) == 1);
    CHECK (e.totalActiveVoices() == 1);

    mixer::MasterSettings m;
    m.maxVoices = 3;
    e.setMasterSettings (m);
    for (int k = 0; k < 8; ++k)
    {
        e.noteOnTrack (0, 0, 40 + k, 0.7f);
        e.process (out, 2, 256);
    }
    for (int i = 0; i < 20; ++i) e.process (out, 2, 256);
    CHECK (e.activeVoices (0) <= 4); // limite 3 + éventuellement une voix en fin de fondu

    // état du mixeur : la description FM fait l'aller-retour
    MixerState s = MixerState::freshProject();
    s.tracks[0].type = TrackType::Fm;
    s.tracks[0].fm.patch.setGlobal (kAlgorithmOffset, 9);
    s.tracks[0].fm.patch.setName ("SAVED");
    s.tracks[0].fm.unisonVoices = 3;
    const auto back = mixerFromJson (toJson (s));
    CHECK (back.tracks[0].type == TrackType::Fm);
    CHECK (back.tracks[0].fm == s.tracks[0].fm);
    CHECK (back.tracks[0].fm.patch.name() == "SAVED");
}

#include "neptune/core/FmPreset.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"

TEST_CASE ("fm presets: keys, capture and apply on any track, factory patches sound", "[fm][preset]")
{
    ParameterSet set;
    REQUIRE (probe::buildPool (set, mixer::numTracks));
    REQUIRE (sampler::addParams (set));
    REQUIRE (mixer::addParams (set, mixer::numTracks, mixer::numBuses));
    for (const auto& k : keyedParams())
        for (int t : { 0, 1, 7, 15 }) CHECK (set.indexOf (idForTrack (k.id, t)) != ParameterSet::npos);

    const auto cfg = defaultConfig();
    set.set (mixer::fmParam (3, mixer::kFmBrightness), 35.0);
    set.set (mixer::fmParam (3, mixer::kFmLevelOp2), -12.0);
    FmDescription d;
    d.patch.setName ("PRESET");
    d.unisonVoices = 4;
    const auto p = capturePreset (set, d, cfg, "Mon FM", "Pad", nullptr, 3);
    CHECK (p.module == moduleId);
    CHECK (p.parameters.at ("fm.brightness").number == 35.0);
    CHECK (p.parameters.at ("fm.op2_level").number == -12.0);

    FmDescription current;
    set.set (mixer::fmParam (9, mixer::kFmBrightness), 99.0);
    applyPresetValues (p, set, cfg, 9);
    current = presetDescription (p, current, cfg);
    CHECK (set.get (mixer::fmParam (9, mixer::kFmBrightness)) == 35.0);
    CHECK (set.get (mixer::fmParam (9, mixer::kFmLevelOp2)) == -12.0);
    CHECK (set.get (mixer::fmParam (8, mixer::kFmBrightness)) == 0.0); // les autres pistes ne bougent pas
    CHECK (current.unisonVoices == 4);
    CHECK_FALSE (differsFromPreset (p, set, current, cfg, 9));
    set.set (mixer::fmParam (9, mixer::kFmEnvSpeed), 10.0); // une macro absente du preset n'est pas « modifiée »
    set.set (mixer::fmParam (9, mixer::kFmBrightness), 36.0);
    CHECK (differsFromPreset (p, set, current, cfg, 9));

    // import d'un patch seul : macros ramenées à leur défaut
    const auto imported = presetFromPatch (Patch::initVoice(), "Import");
    applyPresetValues (imported, set, cfg, 9);
    CHECK (set.get (mixer::fmParam (9, mixer::kFmBrightness)) == 0.0);
    CHECK (set.get (mixer::fmParam (9, mixer::kFmUnisonDetune)) == 15.0); // le défaut du paramètre

    // chaque patch livré sonne
    for (const auto& f : factoryPresets())
    {
        Rig rig;
        rig.desc = fmFromJson (f.state);
        rig.apply();
        rig.voices.noteOn (0, 60, 0.8f);
        const auto out = rig.render (24000);
        INFO (f.name);
        CHECK (out.finite());
        CHECK (out.peak() > 0.02f);
        CHECK (f.id.rfind ("factory-fm6op-", 0) == 0);
    }
}

TEST_CASE ("fm keyboard level scaling follows the break point", "[fm][patch]")
{
    Patch p = Patch::initVoice();
    p.setOp (5, kBreakPoint, 39);   // l'échelle de msfa démarre à la note breakpoint + 17 (ici 56)
    p.setOp (5, kRightDepth, 50);
    p.setOp (5, kRightCurve, 0);    // -LIN : atténue vers l'aigu
    p.setOp (5, kLeftDepth, 0);
    initTables (kRate);
    const int atBreak = keyboardLevelScaling (p, 5, 56);
    const int high = keyboardLevelScaling (p, 5, 100);
    const int low = keyboardLevelScaling (p, 5, 30);
    CHECK (high < atBreak);
    CHECK (low == 0);
    CHECK (atBreak == 0);
}

#include "neptune/core/FmSynth.h"

TEST_CASE ("fm synth engine: plays, volume and pan, state round trip", "[fm][engine]")
{
    FmSynthEngine e;
    REQUIRE (e.ok());
    e.prepare (kRate, 128);
    e.postNoteOn (60, 0.9f);
    std::vector<float> l (128), r (128);
    float peak = 0.0f;
    for (int i = 0; i < 100; ++i)
    {
        e.process (l.data(), r.data(), 128);
        for (auto v : l) peak = std::max (peak, std::fabs (v));
    }
    CHECK (peak > 0.02f);
    CHECK (e.voiceCount() == 1);
    CHECK (e.heldKeyWord (0) == (std::uint64_t (1) << 60));
    e.postNoteOff (60);
    e.postPanic();
    for (int i = 0; i < 200; ++i) e.process (l.data(), r.data(), 128);
    CHECK (e.voiceCount() == 0);

    // pan tout à gauche : la droite est muette
    FmSynthEngine g;
    g.params().set (synthPan, -100.0);
    g.prepare (kRate, 128);
    g.postNoteOn (64, 0.9f);
    float lp = 0.0f, rp = 0.0f;
    for (int i = 0; i < 100; ++i)
    {
        g.process (l.data(), r.data(), 128);
        if (i > 40) for (int k = 0; k < 128; ++k) { lp = std::max (lp, std::fabs (l[static_cast<std::size_t> (k)])); rp = std::max (rp, std::fabs (r[static_cast<std::size_t> (k)])); }
    }
    CHECK (lp > 0.02f);
    CHECK (rp < 1.0e-4f);

    // état
    FmSynthEngine a;
    a.params().set (mixer::fmParam (0, mixer::kFmBrightness), 40.0);
    FmDescription d;
    d.patch.setName ("STATE");
    d.unisonVoices = 3;
    const auto text = stateToJson (a.params(), d, "preset-id-x");
    ParameterSet set;
    REQUIRE (addSynthParams (set));
    FmDescription back;
    std::string id;
    REQUIRE (stateFromJson (text, set, back, id));
    CHECK (back == sanitised (d));
    CHECK (id == "preset-id-x");
    CHECK (set.get (mixer::fmParam (0, mixer::kFmBrightness)) == 40.0);
    CHECK_FALSE (stateFromJson (R"({"format":"analogsynth-state"})", set, back, id));
}

TEST_CASE ("fm voice parameters follow the description and the other way round", "[fm][engine][extra]")
{
    FmSynthEngine e;
    REQUIRE (e.ok());
    FmDescription d;
    d.patch.setGlobal (kLfoOffset, 61);
    d.patch.setGlobal (kLfoOffset + 2, 40);
    d.pitchBendRange = 7;
    d.glideMs = 300;
    e.setDescription (d);
    auto value = [&e] (mixer::FmExtraKind k) { return e.params().get (mixer::fmExtraParam (0, k)); };
    CHECK (value (mixer::kFmLfoSpeed) == 61.0);
    CHECK (value (mixer::kFmLfoPmd) == 40.0);
    CHECK (value (mixer::kFmBendRange) == 7.0);
    CHECK (value (mixer::kFmGlide) == 300.0);

    // l'automation d'un paramètre se retrouve dans la description lue ensuite, et dans l'état enregistré
    e.params().set (mixer::fmExtraParam (0, mixer::kFmWheelPitch), 9.0);
    CHECK (e.description().wheelPitchSens == 9);
    const auto text = stateToJson (e.params(), e.description(), "");
    ParameterSet set;
    REQUIRE (addSynthParams (set));
    FmDescription back;
    std::string id;
    REQUIRE (stateFromJson (text, set, back, id));
    CHECK (back.wheelPitchSens == 9);
    CHECK (set.get (mixer::fmExtraParam (0, mixer::kFmWheelPitch)) == 9.0);
}

TEST_CASE ("fm pitch bend range comes from the track parameter", "[fm][engine][extra]")
{
    // même note, même molette de pitch à fond ; l'étendue (paramètre) 0 : la hauteur ne bouge pas, 12 : elle monte d'une octave
    auto frequencyWith = [] (double range)
    {
        FmSynthEngine e;
        e.prepare (kRate, 128);
        e.params().set (mixer::fmExtraParam (0, mixer::kFmBendRange), range);
        e.setPitchBend (16383);
        e.postNoteOn (57, 0.9f);
        std::vector<float> l (128), r (128), all;
        for (int i = 0; i < 80; ++i)
        {
            e.process (l.data(), r.data(), 128);
            if (i >= 20) all.insert (all.end(), l.begin(), l.end());
        }
        int crossings = 0;
        for (std::size_t i = 1; i < all.size(); ++i)
            if (all[i - 1] <= 0.0f && all[i] > 0.0f) ++crossings;
        return static_cast<double> (crossings);
    };
    const double flat = frequencyWith (0.0), up = frequencyWith (12.0);
    CHECK (flat > 10.0);
    CHECK (up > flat * 1.7); // une octave plus haut : environ le double de passages par zéro
}

TEST_CASE ("fm state saved, reloaded and saved again is identical", "[fm][engine][extra]")
{
    FmSynthEngine e;
    e.params().set (mixer::fmExtraParam (0, mixer::kFmGlide), 99.5);
    e.params().set (mixer::fmExtraParam (0, mixer::kFmLfoSpeed), 77.0);
    e.params().set (mixer::fmExtraParam (0, mixer::kFmBendRange), 9.0);
    const auto first = stateToJson (e.params(), e.description(), "");
    FmSynthEngine f;
    FmDescription d;
    std::string id;
    REQUIRE (stateFromJson (first, f.params(), d, id));
    f.setDescription (d);
    CHECK (stateToJson (f.params(), f.description(), "") == first);
}

TEST_CASE ("fm track in the engine: extreme operator level offsets stay silent or bounded (no shift overflow in the exponential table)", "[fm][engine]")
{
    auto peakAt = [] (double level)
    {
        ParameterSet params;
        REQUIRE (sampler::addParams (params));
        REQUIRE (probe::buildPool (params, mixer::numTracks));
        REQUIRE (mixer::addParams (params, mixer::numTracks, mixer::numBuses));
        Engine e;
        e.attachParameters (&params);
        e.prepare (kRate, 256);
        std::vector<float> l (256), r (256);
        float* out[2] = { l.data(), r.data() };
        e.setTrackActive (0, true);
        e.setTrackType (0, TrackType::Fm);
        e.publishFm (0, std::make_shared<FmDescription>());
        for (int op = 0; op < kOperators; ++op) params.set (mixer::fmParam (0, static_cast<mixer::FmKind> (mixer::kFmLevelOp1 + op)), level);
        e.process (out, 2, 256);
        e.noteOnTrack (0, 0, 60, 0.8f);
        float peak = 0.0f;
        for (int i = 0; i < 40; ++i)
        {
            e.process (out, 2, 256);
            peak = 0.0f;
            for (float v : l) peak = std::max (peak, std::fabs (v));
        }
        return peak;
    };
    CHECK (peakAt (-99.0) < 0.001f);   // tous les niveaux au plus bas : le silence (avant : une crête de 3, décalage indéfini)
    CHECK (peakAt (-50.0) < peakAt (0.0));
    const float high = peakAt (99.0);
    CHECK (std::isfinite (high));
    CHECK (high < 16.0f);
}

TEST_CASE ("fm synth engine follows the host modulation without changing the set value", "[fm][engine]")
{
    auto peakOf = [] (FmSynthEngine& e)
    {
        e.prepare (kRate, 128);
        e.postNoteOn (60, 0.9f);
        std::vector<float> l (128), r (128);
        float peak = 0.0f;
        for (int i = 0; i < 100; ++i)
        {
            e.process (l.data(), r.data(), 128);
            for (auto v : l) peak = std::max (peak, std::fabs (v));
        }
        return peak;
    };
    FmSynthEngine plain, modulated;
    const float peakPlain = peakOf (plain);
    const auto idx = modulated.params().indexOf (synthVolume);
    REQUIRE (idx != ParameterSet::npos);
    modulated.params().setHostModOffsetAt (idx, -0.1f);                  // décalage normalisé de la modulation d'hôte
    CHECK (peakOf (modulated) < peakPlain * 0.6f);
    CHECK (modulated.params().get (synthVolume) == plain.params().get (synthVolume));   // le réglage ne bouge pas
}

TEST_CASE ("fm state: the modulation matrix is saved when it is not empty, and an empty one leaves the state unchanged", "[fm][modulation]")
{
    FmSynthEngine e;
    REQUIRE (e.ok());
    const auto without = stateToJson (e.params(), e.description(), "");
    modulation::Matrix empty;
    CHECK (stateToJson (e.params(), e.description(), "", &empty) == without);   // sans matrice : identique octet pour octet à l'état d'avant

    modulation::Matrix m;
    m.lfos[0].rateHz = 3.5;
    modulation::Route r;
    r.source = { modulation::SourceKind::ControlChange, 1, 0 };   // la molette de modulation
    r.dest = synthVolume;
    r.amount = -0.4;
    m.routes.push_back (r);
    const auto with = stateToJson (e.params(), e.description(), "", &m);
    CHECK (with != without);

    FmSynthEngine other;
    REQUIRE (other.ok());
    FmDescription back;
    std::string id;
    modulation::Matrix loaded;
    REQUIRE (stateFromJson (with, other.params(), back, id, &loaded));
    CHECK (loaded == m);
    modulation::Matrix none;
    none.lfos[1].rateHz = 3.0;   // une valeur de départ non vide doit être remise à vide par un état sans matrice
    REQUIRE (stateFromJson (without, other.params(), back, id, &none));
    CHECK (none == modulation::Matrix {});
    CHECK (stateFromJson (without, other.params(), back, id));   // l'appel d'avant, sans matrice, marche toujours
}

TEST_CASE ("fm : une clé « carte » dans l'état (réglages de Carte) ne gêne pas la lecture", "[fm][carte]")
{
    FmSynthEngine a;
    FmDescription d;
    d.unisonVoices = 2;
    auto doc = nlohmann::json::parse (stateToJson (a.params(), d, "p2"));
    doc["carte"] = { { "follow", true }, { "motifs", nlohmann::json::array() } };
    ParameterSet set;
    REQUIRE (addSynthParams (set));
    FmDescription back;
    std::string id;
    REQUIRE (stateFromJson (doc.dump(), set, back, id));
    CHECK (back == sanitised (d));
    CHECK (id == "p2");
}
