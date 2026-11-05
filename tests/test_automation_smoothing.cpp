// Automation de la coupure du filtre pendant une note tenue : marches d'un bloc à l'autre (mesure).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "neptune/core/AnalogSynth.h"
#include "neptune/core/SamplerParams.h"

using namespace neptune::core;
using namespace neptune::core::analogsynth;

namespace
{
constexpr double kRate = 48000.0;

// Note tenue (saw, filtre 1 résonant) ; la coupure monte de 300 à 3000 Hz (exponentiel) sur `seconds`, réécrite avant chaque bloc de `block`
std::vector<float> renderRamp (int block, double seconds)
{
    AnalogSynthEngine e;
    e.prepare (kRate, 256);
    e.params().set (sampler::filter1Resonance, 60.0);
    e.params().set (sampler::filter1Cutoff, 300.0);
    e.noteOn (0, 40, 0.8f);
    const int total = static_cast<int> (seconds * kRate);
    std::vector<float> out;
    std::vector<float> l (static_cast<std::size_t> (block)), r (static_cast<std::size_t> (block));
    for (int done = 0; done < total; done += block)
    {
        const double t = static_cast<double> (done) / total;
        e.params().set (sampler::filter1Cutoff, 300.0 * std::pow (10.0, t));
        e.process (l.data(), r.data(), block);
        out.insert (out.end(), l.begin(), l.end());
    }
    return out;
}

double rms (const std::vector<float>& a, const std::vector<float>& b)
{
    double s = 0.0;
    const std::size_t n = std::min (a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) { const double d = a[i] - b[i]; s += d * d; }
    return std::sqrt (s / static_cast<double> (n));
}

double rmsOf (const std::vector<float>& a)
{
    double s = 0.0;
    for (float v : a) s += static_cast<double> (v) * v;
    return std::sqrt (s / static_cast<double> (a.size()));
}
} // namespace

TEST_CASE ("analogsynth cutoff automation: staircase measurement", "[automation][!mayfail]")
{
    const auto fine = renderRamp (16, 1.5);   // référence : une valeur toutes les 16 échantillons (pas du filtre)
    std::printf ("\n--- coupure automatisée, écart à la référence (blocs de 16), rapport au niveau du signal ---\n");
    for (int block : { 32, 64, 128, 256, 512, 1024 })
    {
        const auto v = renderRamp (block, 1.5);
        std::printf ("bloc %5d : %.4f\n", block, rms (fine, v) / rmsOf (fine));
    }
    SUCCEED();
}

namespace
{
// Une note tenue, puis `id` passe de `from` à `to` (exponentiel) entre 0,5 s et 0,5 s + `rampSec` (saut si rampSec = 0), réécrit avant chaque bloc
std::vector<float> renderMove (ParamId id, double from, double to, double rampSec, int block)
{
    AnalogSynthEngine e;
    e.prepare (kRate, 256);
    e.params().set (sampler::filter1Resonance, 60.0);
    e.params().set (sampler::filter1Cutoff, 300.0);
    e.params().set (id, from);
    e.noteOn (0, 40, 0.8f);
    const int total = static_cast<int> (1.0 * kRate), t0 = static_cast<int> (0.5 * kRate);
    std::vector<float> out, l (static_cast<std::size_t> (block)), r (static_cast<std::size_t> (block));
    for (int done = 0; done < total; done += block)
    {
        double v = from;
        if (done >= t0) v = rampSec <= 0.0 ? to : from * std::pow (to / from, std::min (1.0, (done - t0) / (rampSec * kRate)));
        e.params().set (id, v);
        e.process (l.data(), r.data(), block);
        out.insert (out.end(), l.begin(), l.end());
    }
    return out;
}

double maxStep (const std::vector<float>& x, int a, int b)
{
    double m = 0.0;
    for (int i = std::max (1, a); i < b && i < static_cast<int> (x.size()); ++i) m = std::max (m, static_cast<double> (std::fabs (x[static_cast<std::size_t> (i)] - x[static_cast<std::size_t> (i - 1)])));
    return m;
}
} // namespace

TEST_CASE ("analogsynth parameter moves: fast ramp and jump measurement", "[automation][!mayfail]")
{
    struct Case { const char* name; ParamId id; double from, to, ramp; };
    const Case cases[] = {
        { "coupure 300->3000 en 50 ms", sampler::filter1Cutoff, 300.0, 3000.0, 0.05 },
        { "coupure 300->3000 saut     ", sampler::filter1Cutoff, 300.0, 3000.0, 0.0 },
        { "coupure 3000->300 saut     ", sampler::filter1Cutoff, 3000.0, 300.0, 0.0 },
        { "resonance 10->90 saut      ", sampler::filter1Resonance, 10.0, 90.0, 0.0 },
    };
    std::printf ("\n--- mouvements rapides : pic de saut d'échantillon à échantillon dans les 10 ms après le départ, rapport au pic des 100 ms d'avant ---\n");
    for (const auto& c : cases)
    {
        std::printf ("%s :", c.name);
        const int t0 = static_cast<int> (0.5 * kRate);
        for (int block : { 16, 64, 256, 1024 })
        {
            const auto v = renderMove (c.id, c.from, c.to, c.ramp, block);
            const double before = maxStep (v, t0 - 4800, t0), after = maxStep (v, t0, t0 + 480 + static_cast<int> (c.ramp * kRate));
            std::printf ("  bloc %4d : %.2f", block, after / before);
        }
        std::printf ("\n");
    }
    SUCCEED();
}

namespace
{
void writeWav (const std::string& path, const std::vector<float>& x)
{
    std::FILE* f = std::fopen (path.c_str(), "wb");
    if (f == nullptr) return;
    const std::uint32_t n = static_cast<std::uint32_t> (x.size()) * 2, rate = 48000, byteRate = rate * 2, riff = 36 + n, fmtLen = 16;
    const std::uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite ("RIFF", 1, 4, f); std::fwrite (&riff, 4, 1, f); std::fwrite ("WAVEfmt ", 1, 8, f); std::fwrite (&fmtLen, 4, 1, f);
    std::fwrite (&pcm, 2, 1, f); std::fwrite (&ch, 2, 1, f); std::fwrite (&rate, 4, 1, f); std::fwrite (&byteRate, 4, 1, f);
    std::fwrite (&align, 2, 1, f); std::fwrite (&bits, 2, 1, f); std::fwrite ("data", 1, 4, f); std::fwrite (&n, 4, 1, f);
    for (float v : x) { const auto q = static_cast<std::int16_t> (std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f)); std::fwrite (&q, 2, 1, f); }
    std::fclose (f);
}
} // namespace

// Écoute : NEPTUNE_DUMP_DIR=<dossier> écrit les rendus en WAV
TEST_CASE ("analogsynth cutoff automation: write listening files", "[automation-wav]")
{
    const char* dir = std::getenv ("NEPTUNE_DUMP_DIR");
    if (dir == nullptr) { SUCCEED(); return; }
    const std::string d = dir;
    writeWav (d + "/coupure_montee_lente_bloc16.wav", renderRamp (16, 3.0));
    writeWav (d + "/coupure_montee_lente_bloc1024.wav", renderRamp (1024, 3.0));
    writeWav (d + "/coupure_montee_rapide_bloc16.wav", renderMove (sampler::filter1Cutoff, 300.0, 3000.0, 0.05, 16));
    writeWav (d + "/coupure_montee_rapide_bloc1024.wav", renderMove (sampler::filter1Cutoff, 300.0, 3000.0, 0.05, 1024));
    SUCCEED();
}
