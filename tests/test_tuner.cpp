#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>
#include <vector>

#include "neptune/core/Metronome.h"
#include "neptune/core/SignalGenerator.h"
#include "neptune/core/Tuner.h"

using namespace neptune::core;

namespace
{
constexpr double kTwoPi = 6.283185307179586;

// Fait entendre `hz` (avec des harmoniques) à l'accordeur pendant `seconds` et rend sa lecture.
tuner::PitchTracker::Result hear (double hz, double sr, const std::vector<double>& harmonics, double amp = 0.3, double seconds = 0.6)
{
    tuner::PitchTracker t;
    t.prepare (sr);
    const int n = static_cast<int> (seconds * sr);
    std::vector<float> x (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        double v = 0.0;
        for (std::size_t h = 0; h < harmonics.size(); ++h) v += harmonics[h] * std::sin (kTwoPi * hz * static_cast<double> (h + 1) * i / sr);
        x[static_cast<std::size_t> (i)] = static_cast<float> (amp * v);
    }
    for (int i = 0; i < n; i += 256) t.push (x.data() + i, nullptr, std::min (256, n - i));
    return t.analyse();
}

double centsBetween (double a, double b) { return 1200.0 * std::log2 (a / b); }
}

TEST_CASE ("tuner: note names and frequencies")
{
    CHECK (tuner::noteName (69, false) == "A4");
    CHECK (tuner::noteName (69, true) == "La4");
    CHECK (tuner::noteName (40, false) == "E2");
    CHECK (tuner::noteName (23, false) == "B0");
    CHECK_THAT (tuner::noteFrequency (69.0, 440.0), Catch::Matchers::WithinAbs (440.0, 1.0e-9));
    CHECK_THAT (tuner::noteFrequency (69.0, 432.0), Catch::Matchers::WithinAbs (432.0, 1.0e-9));
    CHECK_THAT (tuner::noteFrequency (40.0), Catch::Matchers::WithinAbs (82.4069, 1.0e-3));
    CHECK_THAT (tuner::frequencyToMidi (440.0, 440.0), Catch::Matchers::WithinAbs (69.0, 1.0e-9));
    CHECK (tuner::frequencyToMidi (0.0) == 0.0);
}

TEST_CASE ("tuner: every tuning has strings in a sensible range, the first one is chromatic")
{
    REQUIRE (tuner::numTunings() >= 20);
    CHECK (tuner::tunings()[0].notes.empty());
    for (std::size_t i = 1; i < tuner::tunings().size(); ++i)
    {
        const auto& t = tuner::tunings()[i];
        CHECK_FALSE (t.notes.empty());
        for (const int n : t.notes) { CHECK (n >= 20); CHECK (n <= 90); }
    }
}

TEST_CASE ("tuner: matching a frequency with the nearest note or string")
{
    // chromatique : La 440 pile, puis 10 cents trop haut
    auto m = tuner::matchFrequency (440.0, 440.0, 0);
    REQUIRE (m.valid);
    CHECK (m.midi == 69);
    CHECK_THAT (m.cents, Catch::Matchers::WithinAbs (0.0, 1.0e-6));
    m = tuner::matchFrequency (440.0 * std::pow (2.0, 10.0 / 1200.0), 440.0, 0);
    CHECK_THAT (m.cents, Catch::Matchers::WithinAbs (10.0, 1.0e-6));
    // guitare standard : 110 Hz = La 2 = corde 2 (index 1)
    m = tuner::matchFrequency (110.0, 440.0, 1);
    CHECK (m.stringIndex == 1);
    CHECK (m.midi == 45);
    // basse 5 cordes : le Si grave (30,87 Hz)
    int bass5 = -1;
    for (int i = 0; i < tuner::numTunings(); ++i) if (std::string (tuner::tunings()[static_cast<std::size_t> (i)].en).find ("Bass - 5") == 0) bass5 = i;
    REQUIRE (bass5 > 0);
    m = tuner::matchFrequency (30.87, 440.0, bass5);
    CHECK (m.stringIndex == 0);
    CHECK (m.midi == 23);
    // ukulélé soprano : le sol aigu en premier
    int uke = -1;
    for (int i = 0; i < tuner::numTunings(); ++i) if (std::string (tuner::tunings()[static_cast<std::size_t> (i)].en).find ("Ukulele - soprano") == 0) uke = i;
    REQUIRE (uke > 0);
    m = tuner::matchFrequency (392.0, 440.0, uke);
    CHECK (m.stringIndex == 0);
    CHECK (m.midi == 67);
    // un autre La de référence déplace tout
    m = tuner::matchFrequency (432.0, 432.0, 0);
    CHECK (m.midi == 69);
    CHECK_THAT (m.cents, Catch::Matchers::WithinAbs (0.0, 1.0e-6));
    CHECK_FALSE (tuner::matchFrequency (-1.0, 440.0, 0).valid);
}

TEST_CASE ("tuner: pitch detection finds the fundamental from bass to ukulele, at several sample rates")
{
    const std::vector<double> sine { 1.0 };
    const std::vector<double> string { 1.0, 0.7, 0.5, 0.35, 0.2 };      // corde pincée : harmoniques décroissantes
    const std::vector<double> weak { 0.35, 1.0, 0.6, 0.3 };             // fondamentale faible (bois, basse électrique)
    for (const double sr : { 44100.0, 48000.0, 96000.0 })
        for (const double hz : { 30.87, 41.20, 55.0, 82.41, 110.0, 196.0, 329.63, 440.0, 659.26 })
        {
            for (const auto* h : { &sine, &string, &weak })
            {
                const auto r = hear (hz, sr, *h);
                INFO ("sr " << sr << " hz " << hz << " harmonics " << h->size());
                REQUIRE (r.valid);
                CHECK (std::fabs (centsBetween (r.freq, hz)) < 3.0);
                CHECK (r.clarity > 0.8f);
            }
        }
}

TEST_CASE ("tuner: silence, a very quiet signal and noise give no pitch")
{
    CHECK_FALSE (hear (110.0, 48000.0, { 1.0 }, 0.0).valid);
    CHECK_FALSE (hear (110.0, 48000.0, { 1.0 }, 1.0e-4).valid);   // environ -80 dB
    tuner::PitchTracker t;
    t.prepare (48000.0);
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> d (-0.3f, 0.3f);
    std::vector<float> x (48000);
    for (auto& v : x) v = d (rng);
    for (std::size_t i = 0; i < x.size(); i += 256) t.push (x.data() + i, nullptr, static_cast<int> (std::min<std::size_t> (256, x.size() - i)));
    CHECK_FALSE (t.analyse().valid);
}

TEST_CASE ("tuner: a stereo input is averaged, and the analysis needs enough samples first")
{
    tuner::PitchTracker t;
    t.prepare (48000.0);
    CHECK_FALSE (t.analyse().valid);   // rien reçu
    std::vector<float> l (24000), r (24000);
    for (std::size_t i = 0; i < l.size(); ++i) { l[i] = static_cast<float> (0.3 * std::sin (kTwoPi * 220.0 * static_cast<double> (i) / 48000.0)); r[i] = l[i]; }
    for (std::size_t i = 0; i + 128 <= l.size(); i += 128) t.push (l.data() + i, r.data() + i, 128);
    const auto res = t.analyse();
    REQUIRE (res.valid);
    CHECK (std::fabs (centsBetween (res.freq, 220.0)) < 3.0);
    CHECK (res.levelDb < -10.0f);
}

TEST_CASE ("tuner: the reference tone is a clean sine at the asked frequency with soft edges")
{
    tuner::ToneGen g;
    g.prepare (48000.0);
    g.reset();
    std::vector<float> out (48000, 0.0f);
    g.render (out.data(), 24000, true, 220.0, 0.5f);
    g.render (out.data() + 24000, 24000, false, 220.0, 0.5f);
    CHECK (std::fabs (out[0]) < 0.01f);                // fondu d'entrée
    float peak = 0.0f;
    for (int i = 2000; i < 20000; ++i) peak = std::max (peak, std::fabs (out[static_cast<std::size_t> (i)]));
    CHECK_THAT (peak, Catch::Matchers::WithinAbs (0.5, 0.01));
    float tail = 0.0f;
    for (int i = 25000; i < 48000; ++i) tail = std::max (tail, std::fabs (out[static_cast<std::size_t> (i)]));
    CHECK (tail == 0.0f);                              // coupé après le fondu de sortie
}

TEST_CASE ("tuner: the generator engine feeds the tracker and mixes the tone and the click into its output")
{
    generator::GeneratorEngine e;
    REQUIRE (e.ok());
    e.prepare (48000.0, 512);
    std::vector<float> in (48000);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float> (0.3 * std::sin (kTwoPi * 110.0 * static_cast<double> (i) / 48000.0));
    std::vector<float> l (512), r (512);
    for (std::size_t i = 0; i + 512 <= in.size(); i += 512)
    {
        e.pushInput (in.data() + i, nullptr, 512);
        e.process (l.data(), r.data(), 512, generator::Transport {});
    }
    const auto reading = e.tunerReading();
    REQUIRE (reading.valid);
    CHECK (std::fabs (centsBetween (reading.freq, 110.0)) < 3.0);

    // note de référence : La 440
    e.params().set (generator::tunerTone, 1.0);
    float peak = 0.0f;
    for (int b = 0; b < 40; ++b)
    {
        e.process (l.data(), r.data(), 512, generator::Transport {});
        for (const float v : l) peak = std::max (peak, std::fabs (v));
    }
    CHECK (peak > 0.05f);
    e.params().set (generator::tunerTone, 0.0f);
    // métronome
    e.params().set (generator::metroOn, 1.0);
    float clickPeak = 0.0f;
    for (int b = 0; b < 200; ++b)
    {
        e.process (l.data(), r.data(), 512, generator::Transport {});
        for (const float v : l) clickPeak = std::max (clickPeak, std::fabs (v));
    }
    CHECK (clickPeak > 0.05f);
    CHECK (e.metronomeClicks() >= 4);
}

TEST_CASE ("metronome: clicks fall on the beats, the first beat of the bar is accented")
{
    metronome::Metronome m;
    m.prepare (48000.0);
    metronome::Settings s;
    s.run = true;
    s.bpm = 120.0;
    s.beats = 4;
    std::vector<float> out (48000 * 4 + 100);
    for (std::size_t i = 0; i < out.size(); i += 480) m.render (s, {}, out.data() + i, static_cast<int> (std::min<std::size_t> (480, out.size() - i)));
    // le premier clic démarre à l'échantillon 0 ; ensuite un toutes les 24000 échantillons (120 par minute)
    auto windowPeak = [&] (std::size_t from, std::size_t len) { float p = 0.0f; for (std::size_t i = from; i < from + len; ++i) p = std::max (p, std::fabs (out[i])); return p; };
    const float accent = windowPeak (0, 2000);
    const float normal = windowPeak (24000, 2000);
    CHECK (accent > 0.0f);
    CHECK (normal > 0.0f);
    CHECK (accent > normal * 1.5f);
    CHECK (windowPeak (12000, 2000) < 1.0e-3f);          // silence entre deux temps
    CHECK (windowPeak (96000, 2000) > normal * 1.5f);     // le premier temps de la mesure suivante
    CHECK (m.clicks() >= 4);
}

TEST_CASE ("metronome: subdivisions, beat unit and silence when stopped")
{
    metronome::Metronome m;
    m.prepare (48000.0);
    metronome::Settings s;
    s.run = true;
    s.bpm = 120.0;
    s.sub = 2;
    std::vector<float> out (24000);
    m.render (s, {}, out.data(), 24000);
    auto windowPeak = [&] (std::size_t from, std::size_t len) { float p = 0.0f; for (std::size_t i = from; i < from + len; ++i) p = std::max (p, std::fabs (out[i])); return p; };
    CHECK (windowPeak (12000, 2000) > 0.0f);   // la subdivision tombe à mi-temps
    CHECK (windowPeak (12000, 2000) < windowPeak (0, 2000));
    s.run = false;
    m.render (s, {}, out.data(), 24000);
    CHECK (windowPeak (0, 24000) == 0.0f);
    CHECK (m.beatInBar() == -1);
}

TEST_CASE ("metronome: in host sync it follows the transport, is silent when stopped, and does not click in the middle of a beat after a jump")
{
    metronome::Metronome m;
    m.prepare (48000.0);
    metronome::Settings s;
    s.run = true;
    s.hostSync = true;
    std::vector<float> out (4800);
    metronome::HostPos stopped { true, false, 0.0, 120.0 };
    m.render (s, stopped, out.data(), 4800);
    CHECK (*std::max_element (out.begin(), out.end()) == 0.0f);
    // lecture depuis 0,5 noire : pas de clic avant la noire suivante, qui tombe 0,5 noire = 12000 échantillons plus loin à 120
    metronome::HostPos play { true, true, 0.5, 120.0 };
    std::vector<float> long_ (24000);
    m.render (s, play, long_.data(), 24000);
    float early = 0.0f;
    for (int i = 0; i < 11000; ++i) early = std::max (early, std::fabs (long_[static_cast<std::size_t> (i)]));
    float atBeat = 0.0f;
    for (int i = 12000; i < 14000; ++i) atBeat = std::max (atBeat, std::fabs (long_[static_cast<std::size_t> (i)]));
    CHECK (early == 0.0f);
    CHECK (atBeat > 0.0f);
    // à une position qui tombe pile sur un temps, le clic part tout de suite
    metronome::Metronome m2;
    m2.prepare (48000.0);
    metronome::HostPos at { true, true, 4.0, 120.0 };
    std::vector<float> o2 (2000);
    m2.render (s, at, o2.data(), 2000);
    CHECK (*std::max_element (o2.begin(), o2.end()) > 0.0f);
}
