// Digue (limiteur brickwall) : transparence sous le seuil, plafond tenu, latence déclarée, indépendance de la taille de bloc, release,
// anticipation proportionnelle, mode Low Latency.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "neptune/core/LimiterModule.h"

using namespace neptune::core;
using namespace neptune::core::limiter;
using Catch::Approx;

namespace
{
constexpr double kPi = 3.14159265358979323846;

std::unique_ptr<ModuleRunner> make (double rate = 48000.0, int block = 512)
{
    auto r = std::make_unique<ModuleRunner> (std::make_unique<LimiterModule>());
    REQUIRE (r->ok());
    r->prepare (rate, block);
    r->params().set (stock::mix, 100.0);
    return r;
}

void setP (ModuleRunner& r, ParamId id, double v) { r.params().set (id, v); }

// Passe `in` (même signal à gauche et à droite) par le module par blocs de `block` ; rend la voie gauche.
std::vector<float> run (ModuleRunner& r, const std::vector<float>& in, int block = 512)
{
    std::vector<float> out (in.size()), l (static_cast<std::size_t> (block)), rr (static_cast<std::size_t> (block));
    for (std::size_t pos = 0; pos < in.size(); pos += static_cast<std::size_t> (block))
    {
        const std::size_t n = std::min<std::size_t> (static_cast<std::size_t> (block), in.size() - pos);
        std::copy (in.begin() + static_cast<long> (pos), in.begin() + static_cast<long> (pos + n), l.begin());
        rr = l;
        r.process (l.data(), rr.data(), static_cast<int> (n), false);
        std::copy (l.begin(), l.begin() + static_cast<long> (n), out.begin() + static_cast<long> (pos));
    }
    return out;
}

std::vector<float> sine (double hz, double amp, std::size_t n, double rate = 48000.0)
{
    std::vector<float> x (n);
    for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float> (amp * std::sin (2.0 * kPi * hz * static_cast<double> (i) / rate));
    return x;
}

float peakOf (const std::vector<float>& x, std::size_t from = 0)
{
    float p = 0.0f;
    for (std::size_t i = from; i < x.size(); ++i) p = std::max (p, std::abs (x[i]));
    return p;
}

double db (double lin) { return 20.0 * std::log10 (std::max (lin, 1e-12)); }
}

TEST_CASE ("Digue : latence déclarée en temps (441 échantillons à 44,1 kHz, 480 à 48 kHz)", "[limiter]")
{
    CHECK (make (44100.0)->latencySamples() == 441);
    CHECK (make (48000.0)->latencySamples() == 480);
    CHECK (make (96000.0)->latencySamples() == 960);
    auto r = make (44100.0);
    setP (*r, limiter::mode, kLowLatency);
    CHECK (r->latencySamples() == 52);
    auto r48 = make (48000.0);
    setP (*r48, limiter::mode, kLowLatency);
    CHECK (r48->latencySamples() == 56);
    setP (*r48, limiter::mode, kQuality);
    setP (*r48, limiter::lookahead, 5.0);
    CHECK (r48->latencySamples() == 240);
}

TEST_CASE ("Digue : sous le seuil, le signal est retardé et multiplié par le gain de repos, sans autre changement", "[limiter]")
{
    auto r = make();
    setP (*r, threshold, -12.0);
    setP (*r, ceiling, -12.0);   // gain de repos 0 dB
    const auto in = sine (440.0, 0.1, 20000);
    const auto out = run (*r, in);
    const int d = r->latencySamples();
    for (std::size_t i = static_cast<std::size_t> (d); i < in.size(); ++i) REQUIRE (out[i] == in[i - static_cast<std::size_t> (d)]);
}

TEST_CASE ("Digue : le gain de repos est plafond - seuil", "[limiter]")
{
    auto r = make();
    setP (*r, threshold, -24.0);
    setP (*r, ceiling, -12.0);   // +12 dB
    const auto in = sine (440.0, 0.01, 20000);
    const auto out = run (*r, in);
    const int d = r->latencySamples();
    CHECK (peakOf (out, 2000) / peakOf (in) == Approx (std::pow (10.0, 12.0 / 20.0)).margin (1e-3));
    (void) d;
}

TEST_CASE ("Digue : le plafond n'est jamais dépassé (échantillons), quelle que soit la réduction", "[limiter]")
{
    for (double rel : { 6.0, 15.0, 40.0 })
    {
        auto r = make();
        setP (*r, threshold, -24.0);
        setP (*r, ceiling, -6.0);
        setP (*r, release, rel);
        // trains de pics de formes variées par-dessus un signal soutenu
        std::vector<float> in = sine (200.0, 0.3, 48000);
        for (std::size_t i = 5000; i < in.size(); i += 3571) in[i] = (i / 3571) % 2 ? 0.9f : -1.0f;
        for (std::size_t i = 20000; i < 20400; ++i) in[i] = 0.95f * std::sin (static_cast<float> (i) * 0.5f);
        const auto out = run (*r, in);
        CHECK (db (peakOf (out)) <= -6.0 + 0.001);
    }
}

TEST_CASE ("Digue : même sortie quelle que soit la taille de bloc", "[limiter]")
{
    std::vector<float> in = sine (110.0, 0.2, 40000);
    for (std::size_t i = 3000; i < in.size(); i += 4999) in[i] = 0.9f;
    auto a = make(), b = make();
    for (auto* r : { a.get(), b.get() }) { setP (*r, threshold, -18.0); setP (*r, ceiling, -1.0); setP (*r, release, 12.0); }
    const auto oa = run (*a, in, 512);
    const auto ob = run (*b, in, 37);
    for (std::size_t i = 0; i < in.size(); ++i) REQUIRE (oa[i] == ob[i]);
}

TEST_CASE ("Digue : un pic isolé est ramené au plafond avec une rampe qui commence avant lui", "[limiter]")
{
    auto r = make();
    setP (*r, threshold, 0.0);
    setP (*r, ceiling, 0.0);
    std::vector<float> in (30000, 0.2f);   // signal de fond, pour lire la réduction avant le pic
    for (std::size_t i = 10000; i < 10004; ++i) in[i] = 1.0f;   // pas de pic : exactement le plafond
    for (std::size_t i = 20000; i < 20004; ++i) in[i] = 4.0f;   // pic à 12 dB au-dessus
    const auto out = run (*r, in);
    const std::size_t d = static_cast<std::size_t> (r->latencySamples());
    CHECK (out[10000 + d] == 1.0f);
    CHECK (std::abs (out[20000 + d]) == Approx (1.0).margin (1e-6));
    // la descente commence 2 ms (96 échantillons) avant le pic : engagée juste avant lui, absente 100 échantillons avant
    CHECK (out[20000 + d - 1] < 0.19f);
    CHECK (out[20000 + d - 100] == 0.2f);
}

TEST_CASE ("Digue : le gain de repos change en douceur, sans saut de niveau", "[limiter]")
{
    auto r = make();
    setP (*r, threshold, 0.0);
    setP (*r, ceiling, 0.0);
    const auto in = std::vector<float> (24000, 0.1f);
    auto out1 = run (*r, std::vector<float> (in.begin(), in.begin() + 12000));
    setP (*r, threshold, -12.0);   // +12 dB, un saut de gain de 4 d'un coup serait audible comme un clic
    auto out2 = run (*r, std::vector<float> (in.begin() + 12000, in.end()));
    float biggest = 0.0f;
    for (std::size_t i = 1; i < out2.size(); ++i) biggest = std::max (biggest, std::abs (out2[i] - out2[i - 1]));
    CHECK (biggest < 0.02f);
}

TEST_CASE ("Digue : le release suit la valeur réglée (linéaire en dB)", "[limiter]")
{
    for (double rel : { 8.0, 15.0, 26.0 })
    {
        auto r = make();
        setP (*r, threshold, -12.0);
        setP (*r, ceiling, 0.0);   // gain de repos +12 dB : un sinus à 1 demande 12 dB de réduction
        setP (*r, release, rel);
        // un sinus fort soutenu (réduction de 10 dB) puis le silence presque total : le gain remonte au release
        auto in = sine (1000.0, 1.0, 24000);
        in.insert (in.end(), 48000, 0.0f);
        auto probe = sine (1000.0, 0.01, 72000);   // trace faible pour lire le gain après la fin du bruit fort
        for (std::size_t i = 0; i < 24000; ++i) probe[i] = in[i];
        for (std::size_t i = 24000; i < probe.size(); ++i) in[i] = probe[i];
        const auto out = run (*r, in);
        const std::size_t d = static_cast<std::size_t> (r->latencySamples());
        auto gainAt = [&] (std::size_t t)
        {
            double a = 0.0, b = 0.0;
            for (std::size_t i = t; i < t + 480; ++i) { a = std::max (a, static_cast<double> (std::abs (out[i + d]))); b = std::max (b, static_cast<double> (std::abs (in[i]))); }
            return db (a / b);
        };
        const double g1 = gainAt (30000), g2 = gainAt (42000);   // 0,25 s plus tard
        // la réduction se défait à `rel` dB/s : 0,25 s font rel/4 dB
        const double recovered = g2 - g1;
        CHECK (recovered == Approx (rel * 12000.0 / 48000.0).margin (0.15));
    }
}

TEST_CASE ("Digue : Quality à 10 ms se comporte comme les 480 échantillons de la référence, et l'anticipation courte reste sûre", "[limiter]")
{
    for (double ms : { 10.0, 5.0, 1.0 })
    {
        auto r = make();
        setP (*r, threshold, -18.0);
        setP (*r, ceiling, -1.0);
        setP (*r, lookahead, ms);
        std::vector<float> in = sine (300.0, 0.4, 48000);
        for (std::size_t i = 7000; i < in.size(); i += 6007) in[i] = 1.0f;
        const auto out = run (*r, in);
        CHECK (db (peakOf (out)) <= -1.0 + 0.001);
    }
}

TEST_CASE ("Digue : Low Latency tient le plafond et garde le creux ~10 ms après un pic", "[limiter]")
{
    auto r = make();
    setP (*r, limiter::mode, kLowLatency);
    setP (*r, threshold, -12.0);   // gain de repos +12 dB : l'impulsion de 1,0 devient 3,98 et doit être ramenée à 1,0 (0 dB)
    setP (*r, ceiling, 0.0);
    std::vector<float> in (30000, 0.02f);
    for (std::size_t i = 10000; i < 10004; ++i) in[i] = 1.0f;
    const auto out = run (*r, in);
    CHECK (db (peakOf (out)) <= 0.001);
    const std::size_t d = static_cast<std::size_t> (r->latencySamples());
    const double base = 0.02 * std::pow (10.0, 12.0 / 20.0);
    auto g = [&] (std::size_t t) { return out[t + d] / base; };
    // 300 échantillons après le pic le gain reste au creux (maintien de 480 échantillons depuis le pic), il est revenu 1000 échantillons après
    CHECK (g (10300) < 0.3);
    CHECK (g (11500) > 2.0 * g (10300));   // remonté (l'étage lent finit de remonter à 15 dB/s)
}

TEST_CASE ("Digue : Low Latency, le maintien dure 480 échantillons à 48 kHz depuis le pic", "[limiter]")
{
    auto r = make();
    setP (*r, limiter::mode, kLowLatency);
    setP (*r, threshold, -12.0);
    setP (*r, ceiling, 0.0);
    std::vector<float> in (30000, 0.02f);
    in[10000] = 1.0f;   // impulsion isolée
    const auto out = run (*r, in);
    const std::size_t d = static_cast<std::size_t> (r->latencySamples());
    const double base = 0.02 * std::pow (10.0, 12.0 / 20.0);
    auto g = [&] (std::size_t t) { return out[t + d] / base; };
    REQUIRE (g (10000 + 100) < 0.3);   // la limitation est bien engagée
    // creux gardé jusqu'à 479 échantillons après le pic, la remontée commence à 480 (mesure du banc : 479,9)
    CHECK (g (10000 + 479) == Approx (g (10000 + 100)).margin (1e-6));
    CHECK (g (10000 + 480) > g (10000 + 479) + 1e-4);
}

TEST_CASE ("Digue : stéréo liée, un pic à gauche réduit aussi la droite", "[limiter]")
{
    auto r = make();
    setP (*r, threshold, 0.0);
    setP (*r, ceiling, -6.0);
    std::vector<float> l (4000, 0.0f), rr (4000, 0.1f);
    l[2000] = 1.0f;
    r->process (l.data(), rr.data(), 4000, false);
    // le canal droit (constant, 0,1 sous le plafond) est tout de même réduit autour du pic de gauche
    bool reduced = false;
    for (int i = 0; i < 4000; ++i) if (rr[static_cast<std::size_t> (i)] < 0.09f && rr[static_cast<std::size_t> (i)] > 0.0f) reduced = true;
    CHECK (reduced);
}
