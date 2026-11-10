// Compresseur porté de ZL Compressor : directions, styles, plage, wet, maintien, liaison stéréo, clé externe, filtres de clé, écrêteur, delta, latence.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

#include "neptune/core/CompressorModule.h"
#include "neptune/core/CompressorPresets.h"
#include "neptune/core/DynamicsDsp.h"

using namespace neptune::core;
using Catch::Approx;
namespace c = neptune::core::compressor;

namespace
{
constexpr double kRate = 48000.0;

struct CompRig
{
    ModuleRunner runner { std::make_unique<CompressorModule>() };
    std::vector<float> keyL, keyR;
    CompRig()
    {
        REQUIRE (runner.ok());
        runner.prepare (kRate, 256);
    }
    void set (ParamId id, double v) { runner.params().set (id, v); }
    // joue `seconds` de signal constant (L, R) ; rend le dernier échantillon
    std::pair<float, float> hold (float l, float r, double seconds, const float* kl = nullptr, const float* kr = nullptr)
    {
        std::vector<float> a (256), b (256);
        std::pair<float, float> last { 0.0f, 0.0f };
        for (int blk = 0; blk < static_cast<int> (seconds * kRate / 256.0); ++blk)
        {
            std::fill (a.begin(), a.end(), l);
            std::fill (b.begin(), b.end(), r);
            std::vector<float> ka (256, kl != nullptr ? *kl : 0.0f), kb (256, kr != nullptr ? *kr : 0.0f);
            if (kl != nullptr) runner.setKey (ka.data(), kb.data()); else runner.setKey (nullptr, nullptr);
            runner.process (a.data(), b.data(), 256, false);
            last = { a[255], b[255] };
        }
        return last;
    }
    double gainDb (float level, double seconds = 3.0)
    {
        const auto out = hold (level, level, seconds);
        return 20.0 * std::log10 (std::max (1e-9f, out.first / level));
    }
};
} // namespace

TEST_CASE ("compressor: the gain computer matches the textbook curve in each direction", "[compressor]")
{
    dyn::GainComputer g;
    g.set (dyn::Direction::Compress, -32.0, 3.0, 8.0, 0.0, -140.0);
    CHECK (g.eval (-60.0) == Approx (0.0));                        // sous le genou : aucune réduction
    CHECK (g.eval (-6.0) == Approx ((1.0 / 3.0 - 1.0) * (-6.0 + 32.0)).margin (1e-9));   // bien au-dessus : droite de ratio
    CHECK (g.evalOutput (-6.0) == Approx (-6.0 + (1.0 / 3.0 - 1.0) * 26.0));
    CHECK (g.eval (-32.0) < 0.0);                                  // au seuil : déjà dans le genou
    CHECK (g.eval (-32.0) > g.eval (-6.0));
    // continuité aux bords du genou
    CHECK (g.eval (-40.0 + 1e-6) == Approx (0.0).margin (1e-4));
    g.set (dyn::Direction::Expand, -32.0, 3.0, 8.0, 0.0, -140.0);
    CHECK (g.eval (-6.0) == Approx (0.0));
    CHECK (g.eval (-60.0) < -20.0);                                // sous le seuil : atténué
    g.set (dyn::Direction::Inflate, -32.0, 3.0, 8.0, 0.0, -140.0);
    CHECK (g.eval (-6.0) == Approx (0.0));
    CHECK (g.eval (-60.0) < -5.0);                                 // la courbe garde la convention « réduction » ; le gain appliqué est inversé (voir le test des directions)
}

TEST_CASE ("compressor: a steady level above the threshold is reduced by the textbook amount", "[compressor]")
{
    CompRig r;
    CHECK (r.gainDb (0.5f) == Approx ((1.0 / 3.0 - 1.0) * (-6.0206 + 32.0)).margin (0.05));   // Clean : -17,32 dB
    CompRig quiet;
    CHECK (quiet.gainDb (0.001f) == Approx (0.0).margin (0.01));                              // sous le seuil : rien
}

TEST_CASE ("compressor: the four directions act where they should", "[compressor]")
{
    auto gain = [] (int direction, float level)
    {
        CompRig r;
        r.set (c::direction, direction);
        return r.gainDb (level);
    };
    CHECK (gain (0, 0.5f) < -10.0);        // Compress : réduit le fort
    CHECK (gain (0, 0.001f) == Approx (0.0).margin (0.01));
    CHECK (gain (1, 0.001f) > 5.0);        // Inflate : relève le faible
    CHECK (gain (1, 0.5f) == Approx (0.0).margin (0.01));
    CHECK (gain (2, 0.001f) < -20.0);      // Expand : creuse le faible
    CHECK (gain (2, 0.5f) == Approx (0.0).margin (0.01));
    CHECK (gain (3, 0.5f) > 10.0);         // Shape : relève le fort
}

TEST_CASE ("compressor: every style settles on a finite reduction; feedback styles reduce less at steady state", "[compressor]")
{
    double clean = 0.0;
    for (int style = 0; style < 4; ++style)
    {
        CompRig r;
        r.set (c::style, style);
        const double g = r.gainDb (0.5f);
        INFO ("style " << style);
        CHECK (std::isfinite (g));
        CHECK (g < -5.0);
        if (style == 0) clean = g;
        if (style == 2) CHECK (g == Approx (clean).margin (0.05));   // Optical : même courbe, suiveur sur le signal
        if (style == 1 || style == 3) CHECK (g > clean + 3.0);       // Classic / Vocal : la clé voit la sortie, donc moins de réduction
    }
    // Optical et Vocal n'ont pas de sens en expansion : Clean est utilisé à la place
    CompRig r;
    r.set (c::direction, 2);
    r.set (c::style, 3);
    CHECK (r.gainDb (0.001f) < -20.0);
}

TEST_CASE ("compressor: RANGE limits the reduction and WET scales it", "[compressor]")
{
    CompRig a;
    a.set (c::range, 6.0);
    CHECK (a.gainDb (0.5f) == Approx (-6.0).margin (0.05));
    CompRig w;
    w.set (c::wet, 50.0);
    CHECK (w.gainDb (0.5f) == Approx (0.5 * (1.0 / 3.0 - 1.0) * (-6.0206 + 32.0)).margin (0.1));
    CompRig inf;
    inf.set (c::range, 3.0);
    inf.set (c::rangeInf, 1.0);
    CHECK (inf.gainDb (0.5f) < -15.0);
    CompRig m;
    m.set (c::makeup, 6.0206);
    CHECK (m.gainDb (0.5f) == Approx ((1.0 / 3.0 - 1.0) * (-6.0206 + 32.0) + 6.0206).margin (0.1));
}

TEST_CASE ("compressor: HOLD keeps the reduction after the loud part has gone", "[compressor]")
{
    auto reductionAfterBurst = [] (double holdMs)
    {
        CompRig r;
        r.set (c::attack, 0.0);
        r.set (c::release, 0.8);                  // constante de temps (avant la correction du suiveur, 5 ms valaient 5 / 2 pi)
        r.hold (0.5f, 0.5f, 0.3);                 // forte : réduction installée
        std::vector<float> a (256, 0.001f), b (256, 0.001f);
        // 30 ms de faible niveau : sans maintien la réduction est retombée, avec 100 ms de maintien elle reste
        for (int blk = 0; blk < 6; ++blk) r.runner.process (a.data(), b.data(), 256, false);
        return 20.0 * std::log10 (std::max (1e-9f, a[255] / 0.001f));
    };
    CHECK (reductionAfterBurst (0.0) > -3.0);
    CompRig held;
    held.set (c::attack, 0.0);
    held.set (c::release, 0.8);
    held.set (c::hold, 100.0);
    held.hold (0.5f, 0.5f, 0.3);
    std::vector<float> a (256, 0.001f), b (256, 0.001f);
    for (int blk = 0; blk < 6; ++blk) held.runner.process (a.data(), b.data(), 256, false);
    CHECK (20.0 * std::log10 (std::max (1e-9f, a[255] / 0.001f)) < -10.0);
}

TEST_CASE ("compressor: stereo link spreads the reduction, LR mode keeps channels apart", "[compressor]")
{
    CompRig lr;
    lr.set (c::stereoMode, 1);                       // Left Right
    const auto split = lr.hold (0.5f, 0.001f, 3.0);
    CHECK (20.0 * std::log10 (split.first / 0.5f) < -10.0);
    CHECK (20.0 * std::log10 (split.second / 0.001f) == Approx (0.0).margin (0.05));   // l'autre canal n'est pas touché
    CompRig linked;
    linked.set (c::stereoMode, 1);
    linked.set (c::stereoLink, 100.0);
    const auto both = linked.hold (0.5f, 0.001f, 3.0);
    CHECK (20.0 * std::log10 (both.second / 0.001f) < -5.0);                          // liée : l'autre canal est réduit aussi
    CompRig swapped;
    swapped.set (c::stereoMode, 1);
    swapped.set (c::stereoSwap, 1.0);
    const auto sw = swapped.hold (0.5f, 0.001f, 3.0);
    CHECK (20.0 * std::log10 (std::max (1e-9f, sw.second / 0.001f)) < -10.0);         // échange : la réduction du gauche s'applique au droit
    CHECK (20.0 * std::log10 (sw.first / 0.5f) == Approx (0.0).margin (0.05));
}

TEST_CASE ("compressor: the external key drives the compression and LISTEN outputs the filtered key", "[compressor]")
{
    CompRig r;
    r.set (c::externalSide, 1.0);
    const float loudKey = 0.5f;
    const auto withKey = r.hold (0.1f, 0.1f, 3.0, &loudKey, &loudKey);
    CHECK (20.0 * std::log10 (withKey.first / 0.1f) < -10.0);            // clé forte : signal faible réduit
    const float silent = 0.0f;
    CompRig q;
    q.set (c::externalSide, 1.0);
    const auto noKey = q.hold (0.1f, 0.1f, 3.0, &silent, &silent);
    CHECK (20.0 * std::log10 (noKey.first / 0.1f) == Approx (0.0).margin (0.05));

    CompRig l;
    l.set (c::externalSide, 1.0);
    l.set (c::sideOut, 1.0);
    l.set (c::sideGain, 6.0206);
    const float k = 0.25f;
    const auto heard = l.hold (0.1f, 0.1f, 0.1, &k, &k);
    CHECK (heard.first == Approx (0.5f).margin (1e-3f));                  // la clé (0,25) avec +6 dB
}

TEST_CASE ("compressor: side filters shape the key", "[compressor]")
{
    // une clé continue (0 Hz) est supprimée par un passe-haut : plus de réduction
    CompRig r;
    r.set (c::sideEqBypass, 0.0);   // le filtrage de la clé est éteint par défaut
    r.set (c::bandParam (0, c::kStatus), c::kOn);
    r.set (c::bandParam (0, c::kType), c::kHighPass);
    r.set (c::bandParam (0, c::kFreq), 5000.0);
    r.set (c::bandParam (0, c::kOrder), 3);
    CHECK (r.gainDb (0.5f) > -1.0);
    CompRig bypassed;
    bypassed.set (c::bandParam (0, c::kStatus), c::kOn);
    bypassed.set (c::bandParam (0, c::kType), c::kHighPass);
    bypassed.set (c::bandParam (0, c::kFreq), 5000.0);
    bypassed.set (c::sideEqBypass, 1.0);
    CHECK (bypassed.gainDb (0.5f) < -10.0);
    CompRig off;
    off.set (c::bandParam (0, c::kType), c::kHighPass);   // état « Off » (par défaut) : sans effet
    CHECK (off.gainDb (0.5f) < -10.0);
}

TEST_CASE ("compressor: the clipper bounds the output and delta returns what was removed", "[compressor]")
{
    // l'écrêteur tanh : même réglage de compression avec et sans « drive », sur une sinusoïde forte ; la crête avec écrêteur est plus basse
    auto peakOf = [] (double drive)
    {
        CompRig r;
        r.set (c::threshold, -12.0);
        r.set (c::ratio, 2.0);
        r.set (c::clipperDrive, drive);
        r.set (c::attack, 0.0);
        r.set (c::release, 5.0);
        float peak = 0.0f;
        for (int blk = 0; blk < 100; ++blk)
        {
            std::vector<float> a (256), b;
            for (std::size_t i = 0; i < 256; ++i) a[i] = 0.95f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 100.0 * static_cast<double> (blk * 256 + static_cast<int> (i)) / kRate));
            b = a;
            r.runner.process (a.data(), b.data(), 256, false);
            if (blk > 60) for (const float x : a) peak = std::max (peak, std::fabs (x));
        }
        return peak;
    };
    CHECK (peakOf (100.0) < peakOf (0.0));
    CHECK (peakOf (100.0) > 0.1f);

    CompRig d;
    d.set (c::delta, 1.0);
    const auto removed = d.hold (0.5f, 0.5f, 3.0);
    CHECK (removed.first == Approx (0.5f * (1.0f - std::pow (10.0f, -17.32f / 20.0f))).margin (0.01f));   // entrée - traité
}

TEST_CASE ("compressor: latency follows oversampling and positive lookahead, a negative lookahead adds none", "[compressor]")
{
    CompRig r;
    CHECK (r.runner.latencySamples() == 0);
    r.set (c::oversample, 1.0);
    CHECK (r.runner.latencySamples() == Oversampler::kLatency2x);
    r.set (c::oversample, 2.0);
    CHECK (r.runner.latencySamples() == Oversampler::kLatency4x);
    r.set (c::lookahead, 5.0);
    CHECK (r.runner.latencySamples() == Oversampler::kLatency4x + 240);
    r.set (c::lookahead, -5.0);
    CHECK (r.runner.latencySamples() == Oversampler::kLatency4x);
}

TEST_CASE ("compressor: oversampled output is the input delayed by the latency when nothing is compressed", "[compressor]")
{
    CompRig r;
    r.set (c::oversample, 2.0);
    r.set (c::threshold, 0.0);
    r.set (c::ratio, 1.0);
    std::vector<float> in (8192);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = 0.3f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 440.0 * static_cast<double> (i) / kRate));
    std::vector<float> out (in.size());
    for (std::size_t off = 0; off < in.size(); off += 256)
    {
        std::vector<float> a (in.begin() + static_cast<std::ptrdiff_t> (off), in.begin() + static_cast<std::ptrdiff_t> (off) + 256), b = a;
        r.runner.process (a.data(), b.data(), 256, false);
        std::copy (a.begin(), a.end(), out.begin() + static_cast<std::ptrdiff_t> (off));
    }
    const auto lat = static_cast<std::size_t> (r.runner.latencySamples());
    for (std::size_t i = 3000; i < 3100; ++i) CHECK (out[i] == Approx (in[i - lat]).margin (0.01));
}

TEST_CASE ("compressor: RMS branch reacts slower to a burst than the peak branch and mixes in", "[compressor]")
{
    CompRig r;
    r.set (c::rmsOn, 1.0);
    r.set (c::rmsMix, 100.0);
    r.set (c::rmsLength, 100.0);
    const double g = r.gainDb (0.5f);
    CHECK (std::isfinite (g));
    CHECK (g == Approx ((1.0 / 3.0 - 1.0) * (-6.0206 + 32.0)).margin (0.3));   // niveau continu : même réduction
}

TEST_CASE ("compressor: parameters are declared with the documented defaults", "[compressor]")
{
    CompRig r;
    const auto& p = r.runner.params();
    CHECK (p.get (c::threshold) == Approx (-32.0));
    CHECK (p.get (c::ratio) == Approx (3.0));
    CHECK (p.get (c::attack) == Approx (2.0));
    CHECK (p.get (c::release) == Approx (70.0));
    CHECK (p.get (c::wet) == Approx (100.0));
    CHECK (p.find (c::bandParam (7, c::kQ)) != nullptr);
    CHECK (p.find (c::lookahead)->automatable == false);
    CHECK (p.find (c::oversample)->automatable == false);
    auto* m = static_cast<CompressorModule*> (&r.runner.module());
    CHECK (m->curveOutputDb (p, -6.0) == Approx (-6.0 + (1.0 / 3.0 - 1.0) * 26.0).margin (0.01));
}

TEST_CASE ("compressor: side filter response matches the filters in service", "[compressor]")
{
    CompRig r;
    auto& p = r.runner.params();
    CHECK (CompressorModule::sideResponseDb (p, 1000.0, 48000.0) == Approx (0.0).margin (1e-9));   // tous hors service
    p.set (c::bandParam (0, c::kStatus), c::kOn);
    p.set (c::bandParam (0, c::kType), c::kPeak);
    p.set (c::bandParam (0, c::kFreq), 1000.0);
    p.set (c::bandParam (0, c::kGain), 9.0);
    CHECK (CompressorModule::sideResponseDb (p, 1000.0, 48000.0) == Approx (9.0).margin (0.05));
    p.set (c::bandParam (1, c::kStatus), c::kOn);
    p.set (c::bandParam (1, c::kType), c::kHighPass);
    p.set (c::bandParam (1, c::kFreq), 500.0);
    p.set (c::bandParam (1, c::kOrder), 1);   // 12 dB/oct
    CHECK (CompressorModule::sideBandResponseDb (p, 1, 500.0, 48000.0) == Approx (-3.0).margin (0.1));
    CHECK (CompressorModule::sideBandResponseDb (p, 1, 5000.0, 48000.0) == Approx (0.0).margin (0.1));
    p.set (c::bandParam (0, c::kStatus), c::kBypassed);
    CHECK (CompressorModule::sideBandResponseDb (p, 0, 1000.0, 48000.0) == Approx (0.0).margin (1e-9));
}

TEST_CASE ("compressor: factory presets only name declared parameters, within range", "[compressor]")
{
    CompRig r;
    const auto& p = r.runner.params();
    CHECK (compressor::factoryPresets().size() >= 5);
    for (const auto id : compressor::presetParams()) CHECK (p.find (id) != nullptr);
    for (const auto& preset : compressor::factoryPresets())
        for (const auto& [id, v] : preset.values)
        {
            const auto* spec = p.find (id);
            REQUIRE (spec != nullptr);
            CHECK (spec->clamp (v) == Approx (v));
            bool listed = false;
            for (const auto pid : compressor::presetParams()) listed = listed || pid == id;
            CHECK (listed);
        }
}

TEST_CASE ("compressor: analyzer ring is filled only when active, with aligned input, compressed output, final output and key", "[compressor][analyzer]")
{
    CompRig r;
    auto* m = static_cast<CompressorModule*> (&r.runner.module());
    auto& ring = m->analyzer();
    r.set (c::threshold, -32.0);
    r.set (c::ratio, 4.0);
    r.set (c::makeup, 6.0);
    r.hold (0.5f, 0.5f, 0.2);
    CHECK (ring.writePos() == 0);                      // inactif : rien n'est copié
    ring.setActive (true);
    r.hold (0.5f, 0.5f, 2.0);                          // niveau constant : -6 dBFS, bien au-dessus du seuil
    const auto w = ring.writePos();
    REQUIRE (w > 20000);
    const auto start = w - 4800;
    const float pre = analyzerLevelDb (ring, c::kAnPre, start, 4800, MagType::Peak, StereoType::LeftRight);
    const float comp = analyzerLevelDb (ring, c::kAnCompressed, start, 4800, MagType::Peak, StereoType::LeftRight);
    const float post = analyzerLevelDb (ring, c::kAnPost, start, 4800, MagType::Peak, StereoType::LeftRight);
    CHECK (pre == Approx (-6.02).margin (0.05));
    CHECK (comp - pre == Approx ((1.0 / 4.0 - 1.0) * (-6.02 + 32.0)).margin (0.4));   // réduction = niveau compressé - niveau d'entrée
    CHECK (post - comp == Approx (6.0).margin (0.05));                                 // gain de compensation après la copie « compressée »
    const float key = analyzerLevelDb (ring, c::kAnKey, start, 4800, MagType::Peak, StereoType::LeftRight);
    CHECK (key == Approx (-6.02).margin (0.05));
}

TEST_CASE ("analyzer level follows the ZL conventions for peak, RMS and channel types", "[analyzer]")
{
    AnalyzerRing ring (1, 2);
    std::vector<float> l (1000), rr (1000);
    for (int i = 0; i < 1000; ++i) { l[static_cast<std::size_t> (i)] = 0.5f; rr[static_cast<std::size_t> (i)] = 0.0f; }
    const float* ch[2] = { l.data(), rr.data() };
    ring.push (ch, 1000);
    auto db = [&] (MagType m, StereoType s) { return analyzerLevelDb (ring, 0, 0, 1000, m, s); };
    CHECK (db (MagType::Peak, StereoType::Left) == Approx (-6.02).margin (0.01));
    CHECK (db (MagType::Peak, StereoType::Right) < -200.0f);
    CHECK (db (MagType::Peak, StereoType::LeftRight) == Approx (-6.02).margin (0.01));
    CHECK (db (MagType::Peak, StereoType::Mid) == Approx (-12.04).margin (0.01));            // (0,5 + 0) / 2
    CHECK (db (MagType::Peak, StereoType::Side) == Approx (-12.04).margin (0.01));
    CHECK (db (MagType::Peak, StereoType::MidSide) == Approx (-12.04).margin (0.01));
    CHECK (db (MagType::RMS, StereoType::Left) == Approx (-6.02).margin (0.01));
    CHECK (db (MagType::RMS, StereoType::LeftRight) == Approx (-6.02).margin (0.01));         // somme des énergies des deux voies
    CHECK (db (MagType::RMS, StereoType::MidSide) == Approx (-9.03).margin (0.01));           // la même somme, multipliée par 0,5
    CHECK (analyzerLevelDb (ring, 0, 0, 0, MagType::Peak, StereoType::Left) == 0.0f);
}

TEST_CASE ("compressor: analyzer display settings are hidden, not automatable and have the documented defaults", "[compressor][analyzer]")
{
    CompRig r;
    const auto& p = r.runner.params();
    for (const auto id : { c::anTime, c::anMove, c::anMaxDb, c::anMinDb, c::anMag, c::anStereo, c::anShowIn, c::anShowOut, c::anShowRed, c::anShowKey })
    {
        const auto* s = p.find (id);
        REQUIRE (s != nullptr);
        CHECK (s->hidden);
        CHECK_FALSE (s->automatable);
    }
    CHECK (p.get (c::anTime) == 3.0);       // 18 s
    CHECK (p.get (c::sideEqBypass) == 1.0); // clé non filtrée tant qu'on n'allume pas son égaliseur
    CHECK (p.get (c::anMaxDb) == 1.0);      // 0 dB
    CHECK (p.get (c::anMinDb) == 3.0);      // -54 dB
    CHECK (p.get (c::anShowIn) == 1.0);
    CHECK (p.get (c::anShowKey) == 0.0);
}

// ---- source MIDI du sidechain
#include "neptune/core/MidiKey.h"

TEST_CASE ("midi key: velocity maps to level with the shape curve", "[compressor][midikey]")
{
    CHECK (MidiKey::levelFor (127, 0.0) == Approx (1.0f));
    CHECK (MidiKey::levelFor (0, 0.0) == 0.0f);
    CHECK (MidiKey::levelFor (64, 0.0) == Approx (64.0f / 127.0f));              // linéaire au centre
    CHECK (MidiKey::levelFor (64, 100.0) == Approx (std::pow (64.0 / 127.0, 4.0)));   // k = 4
    CHECK (MidiKey::levelFor (64, -100.0) == Approx (std::pow (64.0 / 127.0, 0.25))); // k = 1/4
    CHECK (MidiKey::levelFor (127, 100.0) == Approx (1.0f));
    CHECK (MidiKey::levelFor (200, 0.0) == Approx (1.0f));                        // borné
}

TEST_CASE ("midi key: notes start at their sample offset and are held until released", "[compressor][midikey]")
{
    MidiKey k;
    std::vector<float> out (64, -1.0f);
    k.beginBlock();
    k.push (10, 0x90, 60, 127);
    k.push (40, 0x80, 60, 0);
    k.render (out.data(), 64, 0, 0.0);
    CHECK (out[9] == 0.0f);
    CHECK (out[10] == Approx (1.0f));
    CHECK (out[39] == Approx (1.0f));
    CHECK (out[40] == 0.0f);
    CHECK (! k.noteHeld());

    k.beginBlock();
    k.push (0, 0x90, 60, 64);
    k.render (out.data(), 64, 0, 0.0);
    CHECK (k.noteHeld());
    k.beginBlock();
    k.render (out.data(), 64, 0, 0.0);                                             // tenue d'un bloc à l'autre
    CHECK (out[0] == Approx (64.0f / 127.0f));
    CHECK (out[63] == Approx (64.0f / 127.0f));
}

TEST_CASE ("midi key: overlapping notes keep the strongest, channel filter, all notes off, velocity-0 note-on", "[compressor][midikey]")
{
    MidiKey k;
    std::vector<float> out (16);
    k.beginBlock();
    k.push (0, 0x90, 60, 100);
    k.push (0, 0x90, 64, 40);
    k.render (out.data(), 16, 0, 0.0);
    CHECK (out[0] == Approx (100.0f / 127.0f));
    k.beginBlock();
    k.push (0, 0x80, 60, 0);                                                       // la plus forte relâchée : la plus faible reste
    k.render (out.data(), 16, 0, 0.0);
    CHECK (out[0] == Approx (40.0f / 127.0f));
    k.beginBlock();
    k.push (0, 0x90, 64, 0);                                                       // note-on de vélocité 0 = note-off
    k.render (out.data(), 16, 0, 0.0);
    CHECK (out[0] == 0.0f);

    k.beginBlock();
    k.push (0, 0x91, 60, 127);                                                     // canal 2
    k.render (out.data(), 16, 1, 0.0);                                             // filtre canal 1 : ignoré
    CHECK (out[0] == 0.0f);
    k.beginBlock();
    k.render (out.data(), 16, 2, 0.0);                                             // le canal est relu à chaque bloc
    CHECK (out[0] == Approx (1.0f));
    k.beginBlock();
    k.push (0, 0xB1, 123, 0);                                                      // All notes off
    k.render (out.data(), 16, 0, 0.0);
    CHECK (out[0] == 0.0f);
}

TEST_CASE ("midi key: events of a block that was not rendered are applied by the next beginBlock, and a full queue never loses a note-off", "[compressor][midikey]")
{
    MidiKey k;
    std::vector<float> out (8);
    k.beginBlock();
    k.push (0, 0x90, 60, 127);
    k.beginBlock();            // module contourné : le bloc n'a pas été rendu
    k.render (out.data(), 8, 0, 0.0);
    CHECK (out[0] == Approx (1.0f));
    k.beginBlock();
    for (int i = 0; i < 600; ++i) k.push (0, 0x90, static_cast<std::uint8_t> (i % 100), 10);
    k.push (0, 0x80, 60, 0);
    k.push (0, 0xB0, 123, 0);
    k.render (out.data(), 8, 0, 0.0);
    CHECK (out[0] == 0.0f);
}

TEST_CASE ("compressor: the MIDI source compresses while a note is held, bypasses the key filters and cannot be listened to", "[compressor][midikey]")
{
    auto run = [] (bool noteOn, bool listen, int channel, int noteChannel)
    {
        CompRig r;
        r.set (c::externalSide, c::kSourceMidi);
        r.set (c::sideOut, listen ? 1.0 : 0.0);
        r.set (c::midiChannel, channel);
        // un passe-haut à 5 kHz éteindrait une clé continue s'il s'appliquait : en MIDI il est contourné
        r.set (c::sideEqBypass, 0.0);
        r.set (c::bandParam (0, c::kStatus), c::kOn);
        r.set (c::bandParam (0, c::kType), c::kHighPass);
        r.set (c::bandParam (0, c::kFreq), 5000.0);
        r.set (c::bandParam (0, c::kOrder), 3);
        auto& mk = static_cast<CompressorModule&> (r.runner.module()).midiKey();
        std::vector<float> a (256), b (256);
        std::pair<float, float> last { 0.0f, 0.0f };
        for (int blk = 0; blk < 560; ++blk)
        {
            std::fill (a.begin(), a.end(), 0.1f);
            std::fill (b.begin(), b.end(), 0.1f);
            mk.beginBlock();
            if (blk == 0 && noteOn) mk.push (0, static_cast<std::uint8_t> (0x90 | noteChannel), 60, 127);
            r.runner.process (a.data(), b.data(), 256, false);
            last = { a[255], b[255] };
        }
        return last.first;
    };
    CHECK (20.0 * std::log10 (run (true, false, 0, 0) / 0.1f) < -10.0);                       // note tenue : réduction
    CHECK (20.0 * std::log10 (run (false, false, 0, 0) / 0.1f) == Approx (0.0).margin (0.05)); // pas de note : aucune réduction
    CHECK (20.0 * std::log10 (run (true, false, 3, 0) / 0.1f) == Approx (0.0).margin (0.05));  // note sur le canal 1, filtre canal 3
    CHECK (20.0 * std::log10 (run (true, false, 3, 2) / 0.1f) < -10.0);                        // note sur le canal 3
    CHECK (run (true, true, 0, 0) < 0.1f);                                                      // LISTEN ignoré en MIDI : le signal reste le signal compressé
}

TEST_CASE ("compressor: the MIDI source parameters exist and the old external switch values keep their meaning", "[compressor][midikey]")
{
    CompRig r;
    const auto* src = r.runner.params().find (c::externalSide);
    REQUIRE (src != nullptr);
    CHECK (src->max == Approx (2.0));
    CHECK (src->defaultValue == Approx (0.0));
    REQUIRE (r.runner.params().find (c::midiShape) != nullptr);
    const auto* ch = r.runner.params().find (c::midiChannel);
    REQUIRE (ch != nullptr);
    CHECK (ch->max == Approx (16.0));
    CHECK (! ch->automatable);
}

TEST_CASE ("dynamics follower: attack and release times are time constants (63 % after the set time)", "[compressor]")
{
    neptune::core::dyn::Follower f;
    f.prepare (48000.0);
    f.set (100.0, 250.0, 0.0, 0.0);
    f.reset (0.0);
    double y = 0.0;
    for (int i = 0; i < 4800; ++i) y = f.process (1.0);                     // 100 ms d'échelon vers le haut
    CHECK (y == Approx (1.0 - std::exp (-1.0)).margin (0.01));
    for (int i = 0; i < 48000 * 2; ++i) y = f.process (1.0);                // rattrape l'échelon
    CHECK (y == Approx (1.0).margin (0.001));
    for (int i = 0; i < 12000; ++i) y = f.process (0.0);                    // 250 ms de relâchement
    CHECK (y == Approx (std::exp (-1.0)).margin (0.01));
}
