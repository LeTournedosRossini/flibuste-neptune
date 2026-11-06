// Trident : séparateur de fréquences (Linkwitz-Riley), routage à trois chemins, alignement des latences, paramètres assignables, état.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "neptune/core/Crossover.h"
#include "neptune/core/Trident.h"

using namespace neptune::core;
using namespace neptune::core::trident;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

struct Split { std::vector<float> low, mid, high, in; };

Split runCrossover (int bands, int slope, double f1, double f2, double freq, int samples = 48000)
{
    Crossover c;
    c.prepare (kSr);
    c.setBands (bands);
    c.setSlope (slope);
    c.setFrequencies (f1, f2);
    Split s;
    s.in.resize (static_cast<std::size_t> (samples));
    for (int i = 0; i < samples; ++i) s.in[static_cast<std::size_t> (i)] = static_cast<float> (0.5 * std::sin (2.0 * kPi * freq * i / kSr));
    s.low.assign (s.in.size(), 0.0f);
    s.mid = s.high = s.low;
    std::vector<float> a (s.in.size()), b (s.in.size()), c2 (s.in.size());   // canal droit : mêmes calculs, ignoré
    for (int pos = 0; pos < samples; pos += 480)
    {
        const int n = std::min (480, samples - pos);
        c.process (s.in.data() + pos, s.in.data() + pos, s.low.data() + pos, a.data() + pos, s.mid.data() + pos, b.data() + pos, s.high.data() + pos, c2.data() + pos, n);
    }
    return s;
}

double rms (const std::vector<float>& v, std::size_t from)
{
    double e = 0.0;
    for (std::size_t i = from; i < v.size(); ++i) e += static_cast<double> (v[i]) * v[i];
    return std::sqrt (e / static_cast<double> (v.size() - from));
}
double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }
}

TEST_CASE ("trident : la somme des bandes est plate (2 et 3 bandes, 12 / 24 / 48 dB par octave)", "[trident][crossover]")
{
    for (const int bands : { 2, 3 })
        for (const int slope : { 12, 24, 48 })
            for (const double f : { 60.0, 200.0, 250.0, 700.0, 1500.0, 3000.0, 5000.0, 12000.0 })
            {
                const auto s = runCrossover (bands, slope, 250.0, 3000.0, f);
                std::vector<float> sum (s.in.size());
                for (std::size_t i = 0; i < sum.size(); ++i) sum[i] = s.low[i] + s.mid[i] + s.high[i];
                const double ratio = db (rms (sum, 24000)) - db (rms (s.in, 24000));
                INFO ("bandes " << bands << " pente " << slope << " fréquence " << f);
                CHECK_THAT (ratio, WithinAbs (0.0, 0.1));
            }
}

TEST_CASE ("trident : chaque bande ne garde que sa région", "[trident][crossover]")
{
    // trois bandes, coupures 250 Hz et 3 kHz, pente 24 dB/oct
    const auto lowTone = runCrossover (3, 24, 250.0, 3000.0, 50.0);
    CHECK_THAT (db (rms (lowTone.low, 24000)) - db (rms (lowTone.in, 24000)), WithinAbs (0.0, 0.3));
    CHECK (db (rms (lowTone.mid, 24000)) - db (rms (lowTone.in, 24000)) < -30.0);
    CHECK (db (rms (lowTone.high, 24000)) - db (rms (lowTone.in, 24000)) < -60.0);
    const auto midTone = runCrossover (3, 24, 250.0, 3000.0, 900.0);
    CHECK_THAT (db (rms (midTone.mid, 24000)) - db (rms (midTone.in, 24000)), WithinAbs (0.0, 0.5));
    CHECK (db (rms (midTone.low, 24000)) - db (rms (midTone.in, 24000)) < -30.0);
    CHECK (db (rms (midTone.high, 24000)) - db (rms (midTone.in, 24000)) < -30.0);
    const auto highTone = runCrossover (3, 24, 250.0, 3000.0, 12000.0);
    CHECK_THAT (db (rms (highTone.high, 24000)) - db (rms (highTone.in, 24000)), WithinAbs (0.0, 0.3));
    CHECK (db (rms (highTone.low, 24000)) - db (rms (highTone.in, 24000)) < -60.0);
    // à la fréquence de coupure : chaque côté est à -6 dB (Linkwitz-Riley)
    const auto at = runCrossover (2, 24, 1000.0, 3000.0, 1000.0);
    CHECK_THAT (db (rms (at.low, 24000)) - db (rms (at.in, 24000)), WithinAbs (-6.0, 0.3));
    CHECK_THAT (db (rms (at.high, 24000)) - db (rms (at.in, 24000)), WithinAbs (-6.0, 0.3));
    // à deux bandes, le médium est muet
    CHECK (rms (at.mid, 0) == 0.0);
}

TEST_CASE ("trident : déplacer les coupures ne claque pas", "[trident][crossover]")
{
    Crossover c;
    c.prepare (kSr);
    c.setBands (3);
    c.setSlope (24);
    c.setFrequencies (250.0, 3000.0);
    std::vector<float> in (480), l (480), m (480), h (480), d1 (480), d2 (480), d3 (480);
    double worst = 0.0;
    float prev = 0.0f;
    long t = 0;
    for (int block = 0; block < 400; ++block)
    {
        if (block == 100) c.setFrequencies (800.0, 6000.0);
        if (block == 250) c.setFrequencies (120.0, 1500.0);
        for (int i = 0; i < 480; ++i, ++t) in[static_cast<std::size_t> (i)] = static_cast<float> (0.4 * std::sin (2.0 * kPi * 440.0 * static_cast<double> (t) / kSr));
        c.process (in.data(), in.data(), l.data(), d1.data(), m.data(), d2.data(), h.data(), d3.data(), 480);
        for (int i = 0; i < 480; ++i)
        {
            const float sum = l[static_cast<std::size_t> (i)] + m[static_cast<std::size_t> (i)] + h[static_cast<std::size_t> (i)];
            if (block > 5) worst = std::max (worst, static_cast<double> (std::fabs (sum - prev)));
            prev = sum;
        }
    }
    CHECK (worst < 0.2);   // une sinusoïde d'amplitude 0,4 à 440 Hz avance de 0,23 au plus par échantillon : pas de saut au-delà
}

namespace
{
std::vector<float> noiseL, noiseR;
void makeNoise (std::size_t n)
{
    noiseL.resize (n);
    noiseR.resize (n);
    unsigned s = 12345;
    for (std::size_t i = 0; i < n; ++i)
    {
        s = s * 1664525u + 1013904223u;
        noiseL[i] = (static_cast<float> (s >> 8) / 8388608.0f - 1.0f) * 0.3f;
        s = s * 1664525u + 1013904223u;
        noiseR[i] = (static_cast<float> (s >> 8) / 8388608.0f - 1.0f) * 0.3f;
    }
}
// Fait passer le bruit (par blocs de 256) et rend la sortie.
std::pair<std::vector<float>, std::vector<float>> runTrident (Trident& t, std::size_t n = 24000)
{
    makeNoise (n);
    auto l = noiseL, r = noiseR;
    for (std::size_t pos = 0; pos < n; pos += 256)
    {
        const int m = static_cast<int> (std::min<std::size_t> (256, n - pos));
        t.process (l.data() + pos, r.data() + pos, m);
    }
    return { l, r };
}
void setChoice (Trident& t, int path, ParamId which, int v) { t.params().set (pid::path (path, which), v); }
}

TEST_CASE ("trident : parallèle pur, un seul chemin audible = le signal d'entrée tel quel", "[trident]")
{
    Trident t;
    REQUIRE (t.ok());
    t.prepare (kSr, 256);
    for (int p = 0; p < kPaths; ++p) setChoice (t, p, pid::source, 0);
    t.params().set (pid::path (1, pid::mute), 1.0);
    t.params().set (pid::path (2, pid::mute), 1.0);
    const auto [l, r] = runTrident (t);
    makeNoise (l.size());
    for (std::size_t i = 4000; i < l.size(); ++i) { CHECK_THAT (l[i], WithinAbs (noiseL[i], 1e-6)); CHECK_THAT (r[i], WithinAbs (noiseR[i], 1e-6)); }
}

TEST_CASE ("trident : trois chemins complets s'additionnent ; la polarité inversée annule", "[trident]")
{
    Trident t;
    t.prepare (kSr, 256);
    for (int p = 0; p < kPaths; ++p) setChoice (t, p, pid::source, 0);
    t.params().set (pid::path (2, pid::mute), 1.0);
    const auto sum = runTrident (t).first;
    makeNoise (sum.size());
    for (std::size_t i = 4000; i < sum.size(); ++i) CHECK_THAT (sum[i], WithinAbs (2.0 * noiseL[i], 1e-5));
    t.params().set (pid::path (1, pid::invert), 1.0);
    const auto cancel = runTrident (t).first;
    for (std::size_t i = 8000; i < cancel.size(); ++i) CHECK_THAT (cancel[i], WithinAbs (0.0, 1e-5));
}

TEST_CASE ("trident : grave / médium / aigu sans effet redonnent un signal d'amplitude égale (somme plate)", "[trident]")
{
    Trident t;
    t.prepare (kSr, 256);
    setChoice (t, 0, pid::source, 1);
    setChoice (t, 1, pid::source, 2);
    setChoice (t, 2, pid::source, 3);
    const auto out = runTrident (t, 48000).first;
    makeNoise (out.size());
    CHECK_THAT (db (rms (out, 12000)) - db (rms (noiseL, 12000)), WithinAbs (0.0, 0.2));
}

TEST_CASE ("trident : solo et mute", "[trident]")
{
    Trident t;
    t.prepare (kSr, 256);
    for (int p = 0; p < kPaths; ++p) setChoice (t, p, pid::source, 0);
    t.params().set (pid::path (1, pid::mute), 1.0);
    t.params().set (pid::path (2, pid::mute), 1.0);
    t.params().set (pid::path (0, pid::mute), 1.0);
    t.params().set (pid::path (1, pid::solo), 1.0);   // le solo l'emporte sur les autres, mais un chemin muté reste muet
    auto out = runTrident (t).first;
    for (std::size_t i = 4000; i < out.size(); ++i) CHECK (out[i] == 0.0f);
    t.params().set (pid::path (1, pid::mute), 0.0);
    out = runTrident (t).first;
    makeNoise (out.size());
    for (std::size_t i = 8000; i < out.size(); ++i) CHECK_THAT (out[i], WithinAbs (noiseL[i], 1e-5));
}

TEST_CASE ("trident : un chemin plus lent aligne les autres, et la latence totale est annoncée", "[trident][latency]")
{
    Trident t;
    t.prepare (kSr, 256);
    REQUIRE (t.setEffect (0, 0, "limiter"));
    t.process (noiseL.data(), noiseR.data(), 0);
    for (int p = 0; p < kPaths; ++p) setChoice (t, p, pid::source, 0);
    t.params().set (pid::path (2, pid::mute), 1.0);
    // une impulsion
    std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
    l[100] = r[100] = 0.1f;
    for (std::size_t pos = 0; pos < l.size(); pos += 256) t.process (l.data() + pos, r.data() + pos, 256);
    const int lat = t.latencySamples();
    REQUIRE (lat > 0);
    CHECK (t.pathLatency (0) == lat);
    CHECK (t.pathLatency (1) == 0);
    // le chemin 1 (sans effet) est retardé de `lat` : son impulsion est à 100 + lat, comme celle du chemin 0
    std::size_t peak = 0;
    for (std::size_t i = 0; i < l.size(); ++i) if (std::fabs (l[i]) > std::fabs (l[peak])) peak = i;
    CHECK (peak == 100 + static_cast<std::size_t> (lat));
    // et l'impulsion de ce chemin ne laisse rien juste avant
    CHECK (std::fabs (l[100 + static_cast<std::size_t> (lat) - 1]) < 1e-6f);
}

TEST_CASE ("trident : un paramètre assignable règle la cible", "[trident][assign]")
{
    Trident t;
    t.prepare (kSr, 256);
    REQUIRE (t.setEffect (1, 2, "utility"));
    auto fx = t.effect (1, 2);
    REQUIRE (fx != nullptr);
    fx->runner.params().set (stock::outputTrim, -6.0);
    REQUIRE (t.assign (0, 1, 2, stock::outputTrim));
    // la valeur de l'assignable prend celle de la cible (pas de saut)
    CHECK_THAT (t.params().getNormalizedAt (t.params().indexOf (pid::assign (0))), WithinAbs (fx->runner.params().getNormalized (stock::outputTrim), 1e-9));
    runTrident (t, 512);
    CHECK_THAT (fx->runner.params().get (stock::outputTrim), WithinAbs (-6.0, 1e-9));   // rien n'a bougé : l'assignable n'écrase pas la cible
    t.params().setNormalizedAt (t.params().indexOf (pid::assign (0)), 1.0);               // l'hôte pousse l'assignable
    runTrident (t, 512);
    CHECK_THAT (fx->runner.params().get (stock::outputTrim), WithinAbs (24.0, 1e-9));
    t.unassign (0);
    t.params().setNormalizedAt (t.params().indexOf (pid::assign (0)), 0.0);
    runTrident (t, 512);
    CHECK_THAT (fx->runner.params().get (stock::outputTrim), WithinAbs (24.0, 1e-9));     // plus assigné : la cible ne bouge plus
    // une assignation sur un réglage de Trident
    REQUIRE (t.assign (5, -1, 0, pid::outputGain));
    t.params().setNormalizedAt (t.params().indexOf (pid::assign (5)), 0.0);
    runTrident (t, 512);
    CHECK_THAT (t.params().get (pid::outputGain), WithinAbs (-60.0, 1e-9));
    // une cible inconnue est refusée
    CHECK_FALSE (t.assign (1, 0, 0, stock::outputTrim));   // emplacement vide
    CHECK_FALSE (t.assign (1, 1, 2, 9999));
}

TEST_CASE ("trident : état, aller-retour (réglages, effets, assignations)", "[trident][state]")
{
    Trident a;
    a.prepare (kSr, 256);
    a.params().set (pid::xover1, 321.0);
    a.params().set (pid::bands, 0.0);
    setChoice (a, 2, pid::source, 0);
    REQUIRE (a.setEffect (0, 1, "utility"));
    a.effect (0, 1)->runner.params().set (stock::outputTrim, -3.0);
    REQUIRE (a.assign (7, 0, 1, stock::outputTrim));
    const auto text = a.stateToJson();

    Trident b;
    b.prepare (kSr, 256);
    REQUIRE (b.stateFromJson (text));
    CHECK_THAT (b.params().get (pid::xover1), WithinAbs (321.0, 1e-9));
    CHECK (b.params().get (pid::bands) == 0.0);
    CHECK (b.params().get (pid::path (2, pid::source)) == 0.0);
    REQUIRE (b.effect (0, 1) != nullptr);
    CHECK (b.effect (0, 1)->type == "utility");
    CHECK_THAT (b.effect (0, 1)->runner.params().get (stock::outputTrim), WithinAbs (-3.0, 1e-9));
    CHECK (b.effect (0, 0) == nullptr);
    const auto t = b.assignment (7);
    CHECK (t.active);
    CHECK (t.path == 0);
    CHECK (t.slot == 1);
    CHECK (t.param == stock::outputTrim);
    CHECK_FALSE (b.assignment (8).active);
    // le rappel ne laisse pas l'assignable écraser le réglage rappelé
    runTrident (b, 512);
    CHECK_THAT (b.effect (0, 1)->runner.params().get (stock::outputTrim), WithinAbs (-3.0, 1e-9));
    CHECK_FALSE (b.stateFromJson ("garbage"));
    CHECK_FALSE (b.stateFromJson (R"({"format":"autre"})"));
}
