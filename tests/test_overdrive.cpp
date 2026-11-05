// Fournaise (overdrive à quatre modèles) : courbes, harmoniques, repliement, latence, continu, modes de voies, entrée latérale.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "neptune/core/OverdriveModule.h"
#include "neptune/core/OverdrivePresets.h"

using namespace neptune::core;
using namespace neptune::core::overdrive;
using Catch::Approx;

namespace
{
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::shared_ptr<InsertInstance> make (int block = 512)
{
    auto inst = createInsert ("overdrive", kRate, block);
    REQUIRE (inst != nullptr);
    return inst;
}

void setP (InsertInstance& inst, ParamId id, double v) { inst.runner.params().set (id, v); }

// Passe `in` (même signal à gauche et à droite) par le module en blocs de 512 ; rend la voie gauche.
std::vector<float> run (InsertInstance& inst, const std::vector<float>& in)
{
    std::vector<float> out (in.size());
    std::vector<float> l (512), r (512);
    for (std::size_t pos = 0; pos < in.size(); pos += 512)
    {
        const std::size_t n = std::min<std::size_t> (512, in.size() - pos);
        std::copy (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + n), l.begin());
        r = l;
        inst.runner.process (l.data(), r.data(), static_cast<int> (n), false);
        std::copy (l.begin(), l.begin() + static_cast<long> (n), out.begin() + static_cast<long> (pos));
    }
    return out;
}

std::vector<float> sine (double hz, double amp, double seconds)
{
    std::vector<float> x (static_cast<std::size_t> (seconds * kRate));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float> (amp * std::sin (2.0 * kPi * hz * static_cast<double> (i) / kRate));
    return x;
}

// Amplitude (crête) de la composante à `hz` sur les `n` derniers échantillons (transformée de Fourier en un point).
double component (const std::vector<float>& x, double hz, std::size_t n, double rate = kRate)
{
    double re = 0.0, im = 0.0;
    const std::size_t start = x.size() - n;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double ph = 2.0 * kPi * hz * static_cast<double> (i) / rate;
        re += static_cast<double> (x[start + i]) * std::cos (ph);
        im -= static_cast<double> (x[start + i]) * std::sin (ph);
    }
    return 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (n);
}
double db (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-12)); }
} // namespace

TEST_CASE ("overdrive declares valid parameters and is known to the registry", "[overdrive]")
{
    auto inst = make();
    CHECK (inst->runner.ok());
    CHECK (inst->runner.params().indexOf (drive) != ParameterSet::npos);
    CHECK (inst->runner.params().indexOf (keyHp) != ParameterSet::npos);
    CHECK (inst->runner.params().size() > 50);
    CHECK (createModule ("overdrive") != nullptr);
}

TEST_CASE ("overdrive transfer curves are bounded, monotonic and symmetric when unbiased", "[overdrive]")
{
    for (int m = 0; m < kNumModels; ++m)
    {
        INFO ("modèle " << modelName (m));
        ShapeParams p;
        p.model = m;
        p.feel = 0.0;   // le seuil du fuzz coupe le début de la courbe : monotone quand même, mais pas de pente 1
        double previous = shape (-50.0, p);
        for (double x = -50.0; x <= 50.0; x += 0.05)
        {
            const double y = shape (x, p);
            CHECK (std::isfinite (y));
            CHECK (std::fabs (y) < 1.6);
            CHECK (y >= previous - 1.0e-9);
            previous = y;
            CHECK (shape (-x, p) == Approx (-y).margin (1.0e-9));   // pas de décalage : courbe impaire
        }
        CHECK (shape (0.0, p) == Approx (0.0).margin (1.0e-9));
        // asymétrie : un décalage de polarisation rend la courbe non impaire
        p.bias = 0.6;
        if (m != kTape) CHECK (std::fabs (shape (0.8, p) + shape (-0.8, p)) > 0.01);   // la bande reste symétrique
    }
}

TEST_CASE ("overdrive harmonics: tube makes even harmonics, tape almost only odd ones", "[overdrive]")
{
    const std::vector<float> in = sine (1000.0, 0.25, 1.5);
    auto measure = [&] (int which, double biasPct, double driveDb)
    {
        auto inst = make();
        setP (*inst, model, which);
        setP (*inst, autoGain, 0.0);
        setP (*inst, drive, driveDb);
        setP (*inst, bias, biasPct);
        const auto out = run (*inst, in);
        const std::size_t n = static_cast<std::size_t> (kRate);   // la dernière seconde : un nombre entier de périodes
        return std::array<double, 3> { component (out, 1000.0, n), component (out, 2000.0, n), component (out, 3000.0, n) };
    };
    const auto tube = measure (kTube, 60.0, 12.0);   // drive modéré : les seuils de coupure asymétriques se voient
    const auto tape = measure (kTape, 0.0, 24.0);
    INFO ("lampe : 2e " << db (tube[1] / tube[0]) << " dB, 3e " << db (tube[2] / tube[0]) << " dB ; bande : 2e " << db (tape[1] / tape[0]) << " dB, 3e " << db (tape[2] / tape[0]) << " dB");
    CHECK (db (tube[1] / tube[0]) > -30.0);   // lampe polarisée : 2e harmonique marquée
    CHECK (db (tape[1] / tape[0]) < -70.0);   // bande : presque pas de 2e
    CHECK (db (tape[2] / tape[0]) > -30.0);   // ... mais une 3e nette
}

TEST_CASE ("overdrive oversampling lowers aliasing", "[overdrive]")
{
    // sinus de 5,3 kHz très saturé : le 7e harmonique (37,1 kHz) se replie à 10,9 kHz quand on ne suréchantillonne pas
    const std::vector<float> in = sine (5300.0, 0.5, 1.5);
    auto aliasLevel = [&] (int oversamplingIndex)
    {
        auto inst = make();
        setP (*inst, model, kFuzz);
        setP (*inst, autoGain, 0.0);
        setP (*inst, drive, 40.0);
        setP (*inst, character, 100.0);
        setP (*inst, oversampling, oversamplingIndex);
        const auto out = run (*inst, in);
        return component (out, 10900.0, static_cast<std::size_t> (kRate));
    };
    const double off = aliasLevel (0), x4 = aliasLevel (2);
    CHECK (db (off) - db (x4) > 20.0);
}

TEST_CASE ("overdrive declared latency matches the measured one", "[overdrive]")
{
    for (int index = 0; index < 3; ++index)
    {
        const int expected = Oversampler::latencyFor (index == 0 ? 1 : index == 1 ? 2 : 4);
        auto inst = make();
        setP (*inst, model, kTape);
        setP (*inst, drive, 0.0);
        setP (*inst, autoGain, 0.0);
        setP (*inst, oversampling, index);
        CHECK (inst->runner.latencySamples() == expected);
        std::vector<float> in (4096, 0.0f);
        in[200] = 0.001f;   // assez petite pour que la courbe soit presque linéaire
        const auto out = run (*inst, in);
        std::size_t peak = 0;
        for (std::size_t i = 0; i < out.size(); ++i) if (std::fabs (out[i]) > std::fabs (out[peak])) peak = i;
        INFO ("suréchantillonnage " << index << " : crête en " << peak);
        CHECK (std::abs (static_cast<int> (peak) - (200 + expected)) <= 2);
    }
}

TEST_CASE ("overdrive output has no DC offset even with an asymmetric curve", "[overdrive]")
{
    auto inst = make();
    setP (*inst, model, kTube);
    setP (*inst, bias, 80.0);
    setP (*inst, drive, 20.0);
    const auto out = run (*inst, sine (220.0, 0.3, 2.0));
    double mean = 0.0, sq = 0.0;
    const std::size_t n = static_cast<std::size_t> (kRate);
    for (std::size_t i = out.size() - n; i < out.size(); ++i) { mean += static_cast<double> (out[i]); sq += static_cast<double> (out[i]) * static_cast<double> (out[i]); }
    mean /= static_cast<double> (n);
    CHECK (std::fabs (mean) < 0.01 * std::sqrt (sq / static_cast<double> (n)));
}

TEST_CASE ("overdrive stays finite and bounded at maximum drive on noise", "[overdrive]")
{
    for (int m = 0; m < kNumModels; ++m)
    {
        auto inst = make();
        setP (*inst, model, m);
        setP (*inst, drive, 60.0);
        setP (*inst, character, 100.0);
        setP (*inst, oversampling, 2);
        std::vector<float> in (24000);
        unsigned s = 12345u;
        for (auto& x : in) { s = s * 1664525u + 1013904223u; x = static_cast<float> (static_cast<int> (s >> 8) % 20001 - 10000) / 10000.0f; }
        const auto out = run (*inst, in);
        for (const float y : out) { REQUIRE (std::isfinite (y)); REQUIRE (std::fabs (y) < 8.0f); }
    }
}

TEST_CASE ("overdrive auto gain brings the saturated level back near the input level", "[overdrive]")
{
    for (const int m : { kTube, kTape, kTransistor })
    {
        auto inst = make();
        setP (*inst, model, m);
        setP (*inst, drive, 24.0);
        setP (*inst, feel, 0.0);   // l'affaissement de la lampe et les accentuations changent le niveau selon le signal : hors de la calibration statique
        setP (*inst, autoGain, 1.0);
        const auto in = sine (440.0, 0.25, 1.5);
        const auto out = run (*inst, in);
        const std::size_t n = static_cast<std::size_t> (kRate);
        auto rms = [n] (const std::vector<float>& x) { double s = 0.0; for (std::size_t i = x.size() - n; i < x.size(); ++i) s += static_cast<double> (x[i]) * static_cast<double> (x[i]); return std::sqrt (s / static_cast<double> (n)); };
        INFO ("modèle " << modelName (m));
        CHECK (std::fabs (db (rms (out)) - db (rms (in))) < 2.0);
    }
}

TEST_CASE ("overdrive mid/side modes leave the other channel untouched (delayed by the latency)", "[overdrive]")
{
    const int latency = Oversampler::latencyFor (2);
    const std::size_t total = 8192;
    std::vector<float> l (total), r (total);
    for (std::size_t i = 0; i < total; ++i) { const float s = static_cast<float> (0.3 * std::sin (2.0 * kPi * 440.0 * static_cast<double> (i) / kRate)); l[i] = s; r[i] = -s; }   // côtés purs
    auto inst = make();
    setP (*inst, channels, kMidOnly);
    setP (*inst, oversampling, 1);   // 2x : latence 31
    setP (*inst, drive, 30.0);
    std::vector<float> ol = l, orr = r;
    for (std::size_t pos = 0; pos < total; pos += 512)
        inst->runner.process (ol.data() + pos, orr.data() + pos, 512, false);
    // milieu nul : rien n'est traité, la voie des côtés ressort telle quelle, retardée
    double err = 0.0;
    for (std::size_t i = 2048; i < total; ++i) err = std::max (err, static_cast<double> (std::fabs (ol[i] - l[i - static_cast<std::size_t> (latency)])));
    CHECK (err < 1.0e-4);
}

TEST_CASE ("overdrive key input raises the drive", "[overdrive]")
{
    const std::vector<float> in = sine (440.0, 0.05, 1.0);
    auto rmsWith = [&] (double amountPct)
    {
        auto inst = make();
        setP (*inst, model, kTube);
        setP (*inst, drive, 0.0);
        setP (*inst, autoGain, 0.0);
        setP (*inst, keySource, kKeySide);
        setP (*inst, keyAmount, amountPct);
        std::vector<float> key (512), l (512), r (512);
        std::size_t keyPos = 0;   // clé forte : sinus de 1 kHz à 0,9 (un continu serait retiré par le passe-haut de la clé)
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t pos = 0; pos + 512 <= in.size(); pos += 512)
        {
            std::copy (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + 512), l.begin());
            r = l;
            for (auto& k : key) k = static_cast<float> (0.9 * std::sin (2.0 * kPi * 1000.0 * static_cast<double> (keyPos++) / kRate));
            inst->runner.setKey (key.data(), key.data());
            inst->runner.process (l.data(), r.data(), 512, false);
            if (pos >= 24000) for (const float y : l) { sum += static_cast<double> (y) * static_cast<double> (y); ++count; }
        }
        return std::sqrt (sum / static_cast<double> (count));
    };
    CHECK (rmsWith (100.0) > 3.0 * rmsWith (0.0));
}

TEST_CASE ("overdrive tames the highs compared with a bare waveshaper (less harsh)", "[overdrive]")
{
    // sinus de 220 Hz fortement poussé : énergie des harmoniques entre 3 et 12 kHz, rapportée au fondamental, pour le module et pour la seule courbe statique
    constexpr double f0 = 220.0, amp = 0.5;
    auto bandRatio = [f0] (const std::vector<float>& x, double rate)
    {
        const std::size_t n = static_cast<std::size_t> (rate);   // 1 s : nombre entier de périodes
        const double fundamental = component (x, f0, n, rate);
        double energy = 0.0;
        for (int k = 14; k <= 54; ++k) { const double c = component (x, f0 * k, n, rate); energy += c * c; }
        return 10.0 * std::log10 (std::max (energy, 1.0e-30) / (fundamental * fundamental));
    };
    for (const int m : { kTube, kTransistor, kFuzz })
    {
        INFO ("modèle " << modelName (m));
        const double driveDb = 36.0;
        auto inst = make();
        setP (*inst, model, m);
        setP (*inst, drive, driveDb);
        setP (*inst, autoGain, 0.0);
        setP (*inst, oversampling, 2);
        const auto out = run (*inst, sine (f0, amp, 1.5));
        const std::vector<float> moduleSignal (out.end() - static_cast<long> (kRate), out.end());
        // courbe statique seule, à 192 kHz (aucun filtre) : ce que donnait la première version
        constexpr double hiRate = 192000.0;
        ShapeParams sp;
        sp.model = m;
        const double g = std::pow (10.0, driveDb / 20.0);
        std::vector<float> bare (static_cast<std::size_t> (hiRate));
        for (std::size_t i = 0; i < bare.size(); ++i) bare[i] = static_cast<float> (shape (g * amp * std::sin (2.0 * kPi * f0 * static_cast<double> (i) / hiRate), sp));
        const double moduleRatio = bandRatio (moduleSignal, kRate), bareRatio = bandRatio (bare, hiRate);
        INFO ("module " << moduleRatio << " dB, courbe nue " << bareRatio << " dB");
        CHECK (moduleRatio < bareRatio - 6.0);
    }
}

TEST_CASE ("overdrive Tight keeps a low note from modulating the mids", "[overdrive]")
{
    // deux notes : 60 Hz fort et 1 kHz faible ; les bandes latérales à 1000 +- 60 Hz mesurent l'intermodulation
    std::vector<float> in (static_cast<std::size_t> (2.0 * kRate));
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const double t = static_cast<double> (i) / kRate;
        in[i] = static_cast<float> (0.25 * std::sin (2.0 * kPi * 60.0 * t) + 0.04 * std::sin (2.0 * kPi * 1000.0 * t));
    }
    auto sidebands = [&] (double tightPct)
    {
        auto inst = make();
        setP (*inst, model, kTube);
        setP (*inst, drive, 28.0);
        setP (*inst, autoGain, 0.0);
        setP (*inst, imperfection, 0.0);
        setP (*inst, tight, tightPct);
        setP (*inst, oversampling, 2);
        const auto out = run (*inst, in);
        const std::size_t n = static_cast<std::size_t> (kRate);
        const double carrier = component (out, 1000.0, n), low = component (out, 940.0, n), high = component (out, 1060.0, n);
        return db (std::sqrt (low * low + high * high) / carrier);
    };
    const double without = sidebands (0.0), with = sidebands (100.0);
    INFO ("intermodulation sans Tight " << without << " dB, avec " << with << " dB");
    CHECK (with < without - 6.0);
}

TEST_CASE ("overdrive tube sag lowers the gain under a sustained loud signal", "[overdrive]")
{
    auto level = [&] (double feelPct)
    {
        auto inst = make();
        setP (*inst, model, kTube);
        setP (*inst, drive, 14.0);
        setP (*inst, feel, feelPct);
        setP (*inst, autoGain, 0.0);
        setP (*inst, imperfection, 0.0);
        const auto out = run (*inst, sine (440.0, 0.3, 2.0));
        double s = 0.0;
        for (std::size_t i = out.size() - static_cast<std::size_t> (kRate); i < out.size(); ++i) s += static_cast<double> (out[i]) * static_cast<double> (out[i]);
        return 10.0 * std::log10 (s / kRate);
    };
    CHECK (level (100.0) < level (0.0) - 0.5);
}

TEST_CASE ("overdrive imperfection makes the two channels slightly different", "[overdrive]")
{
    auto spread = [&] (double imperfectionPct)
    {
        auto inst = make();
        setP (*inst, model, kTube);
        setP (*inst, drive, 14.0);
        setP (*inst, imperfection, imperfectionPct);
        const auto in = sine (440.0, 0.3, 1.5);
        std::vector<float> l (512), r (512);
        double maxDiff = 0.0;
        for (std::size_t pos = 0; pos + 512 <= in.size(); pos += 512)
        {
            std::copy (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + 512), l.begin());
            r = l;
            inst->runner.process (l.data(), r.data(), 512, false);
            if (pos >= 24000) for (std::size_t i = 0; i < 512; ++i) maxDiff = std::max (maxDiff, static_cast<double> (std::fabs (l[i] - r[i])));
        }
        return maxDiff;
    };
    CHECK (spread (0.0) < 1.0e-6);
    CHECK (spread (100.0) > 0.005);
}

TEST_CASE ("overdrive factory presets apply cleanly and stay finite", "[overdrive]")
{
    const auto& presets = factoryPresets();
    REQUIRE (presets.size() >= 10);
    for (const auto& preset : presets)
    {
        INFO ("preset " << preset.name);
        auto inst = make();
        for (const auto& [id, value] : preset.values)
        {
            const auto* spec = inst->runner.params().find (id);
            REQUIRE (spec != nullptr);
            CHECK (spec->clamp (value) == Approx (value));   // valeur dans la plage du paramètre
        }
        applyPreset (inst->runner.params(), preset);
        std::vector<float> in (8192);
        unsigned seed = 777u;
        for (auto& x : in) { seed = seed * 1664525u + 1013904223u; x = 0.3f * static_cast<float> (static_cast<int> (seed >> 8) % 2001 - 1000) / 1000.0f; }
        const auto out = run (*inst, in);
        for (const float y : out) { REQUIRE (std::isfinite (y)); REQUIRE (std::fabs (y) < 8.0f); }
    }
}

// Mesure de charge (cachée : `[.bench]`) : temps de calcul de 10 s de signal, blocs de 64, en pourcentage du temps réel pour un cœur.
TEST_CASE ("overdrive CPU cost", "[.bench]")
{
    const char* modelNames[4] = { "tube", "tape", "transistor", "fuzz" };
    const char* osNames[3] = { "off", "2x", "4x" };
    const auto in = sine (440.0, 0.25, 10.0);
    for (int os = 2; os >= 0; --os)
        for (int m = 0; m < 4; ++m)
        {
            if (os != 2 && m != 0) continue;
            auto inst = make (64);
            setP (*inst, model, m);
            setP (*inst, oversampling, os);
            std::vector<float> l (64), r (64);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t pos = 0; pos + 64 <= in.size(); pos += 64)
            {
                std::copy (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + 64), l.begin());
                r = l;
                inst->runner.process (l.data(), r.data(), 64, false);
            }
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            printf ("BENCH %-10s %-3s : %.2f %% d'un coeur\n", modelNames[m], osNames[os], sec / 10.0 * 100.0);
        }
}

TEST_CASE ("overdrive oversampling default is 2x", "[overdrive]")
{
    auto inst = make();
    CHECK (inst->runner.params().get (oversampling) == Approx (1.0));
}

TEST_CASE ("overdrive changing the oversampling during the sound does not click", "[overdrive]")
{
    for (const double mix : { 100.0, 50.0 })
    {
        auto inst = make (64);
        setP (*inst, oversampling, 2);   // 4x
        setP (*inst, stock::mix, mix);
        setP (*inst, drive, 6.0);
        const auto in = sine (1000.0, 0.25, 1.0);
        std::vector<float> out (in.size());
        std::vector<float> l (64), r (64);
        for (std::size_t pos = 0; pos + 64 <= in.size(); pos += 64)
        {
            if (pos == 12800) setP (*inst, oversampling, 0);    // 4x -> Off
            if (pos == 28800) setP (*inst, oversampling, 1);    // Off -> 2x
            std::copy (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + 64), l.begin());
            r = l;
            inst->runner.process (l.data(), r.data(), 64, false);
            std::copy (l.begin(), l.end(), out.begin() + static_cast<long> (pos));
        }
        double base = 0.0, worst = 0.0;
        for (std::size_t i = 2000; i < 12000; ++i) base = std::max (base, static_cast<double> (std::fabs (out[i] - out[i - 1])));
        for (std::size_t i = 12800; i < 36000; ++i) worst = std::max (worst, static_cast<double> (std::fabs (out[i] - out[i - 1])));
        INFO ("mix " << mix << " base step " << base << " worst step " << worst);
        CHECK (worst < base * 1.5);
    }
}

TEST_CASE ("overdrive output does not depend on the block size", "[overdrive]")
{
    const auto in = sine (330.0, 0.3, 2.0);
    auto renderWith = [&] (int block)
    {
        auto inst = make (block);
        setP (*inst, imperfection, 100.0);
        setP (*inst, oversampling, 2);
        std::vector<float> out (in.size());
        std::vector<float> l (static_cast<std::size_t> (block)), r (static_cast<std::size_t> (block));
        for (std::size_t pos = 0; pos < in.size(); pos += static_cast<std::size_t> (block))
        {
            const std::size_t n = std::min<std::size_t> (static_cast<std::size_t> (block), in.size() - pos);
            std::copy (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + n), l.begin());
            r = l;
            inst->runner.process (l.data(), r.data(), static_cast<int> (n), false);
            std::copy (l.begin(), l.begin() + static_cast<long> (n), out.begin() + static_cast<long> (pos));
        }
        return out;
    };
    const auto ref = renderWith (512);
    for (const int block : { 37, 64, 256 })
    {
        const auto y = renderWith (block);
        double err = 0.0, sig = 0.0;
        for (std::size_t i = 4800; i < ref.size(); ++i) { err += std::pow (static_cast<double> (y[i] - ref[i]), 2.0); sig += std::pow (static_cast<double> (ref[i]), 2.0); }
        INFO ("block " << block << " error " << db (std::sqrt (err / sig)) << " dB");
        CHECK (db (std::sqrt (err / sig)) < -100.0);
    }
}

