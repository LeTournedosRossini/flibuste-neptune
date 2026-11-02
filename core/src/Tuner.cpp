#include "neptune/core/Tuner.h"

#include <algorithm>
#include <cmath>

namespace neptune::core::tuner
{
double noteFrequency (double midi, double a4) noexcept { return a4 * std::pow (2.0, (midi - 69.0) / 12.0); }

double frequencyToMidi (double hz, double a4) noexcept
{
    if (! (hz > 0.0) || ! (a4 > 0.0)) return 0.0;
    return 69.0 + 12.0 * std::log2 (hz / a4);
}

std::string noteName (int midi, bool french)
{
    static const char* const en[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* const fr[12] = { "Do", "Do#", "Ré", "Ré#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };
    midi = std::clamp (midi, 0, 127);
    return std::string (french ? fr[midi % 12] : en[midi % 12]) + std::to_string (midi / 12 - 1);
}

const std::vector<Tuning>& tunings()
{
    static const std::vector<Tuning> list = {
        { "Chromatic", "Chromatique", {} },
        { "Guitar - standard (E A D G B E)", "Guitare - standard (Mi La Ré Sol Si Mi)", { 40, 45, 50, 55, 59, 64 } },
        { "Guitar - drop D", "Guitare - drop D (Ré La Ré Sol Si Mi)", { 38, 45, 50, 55, 59, 64 } },
        { "Guitar - drop C", "Guitare - drop C (Do Sol Do Fa La Ré)", { 36, 43, 48, 53, 57, 62 } },
        { "Guitar - half step down (Eb)", "Guitare - un demi-ton plus bas (Mib)", { 39, 44, 49, 54, 58, 63 } },
        { "Guitar - whole step down (D)", "Guitare - un ton plus bas (Ré)", { 38, 43, 48, 53, 57, 62 } },
        { "Guitar - DADGAD", "Guitare - DADGAD (Ré La Ré Sol La Ré)", { 38, 45, 50, 55, 57, 62 } },
        { "Guitar - open G (D G D G B D)", "Guitare - open G (Ré Sol Ré Sol Si Ré)", { 38, 43, 50, 55, 59, 62 } },
        { "Guitar - open D (D A D F# A D)", "Guitare - open D (Ré La Ré Fa# La Ré)", { 38, 45, 50, 54, 57, 62 } },
        { "Guitar - open E (E B E G# B E)", "Guitare - open E (Mi Si Mi Sol# Si Mi)", { 40, 47, 52, 56, 59, 64 } },
        { "Guitar - 7 strings (B E A D G B E)", "Guitare - 7 cordes (Si Mi La Ré Sol Si Mi)", { 35, 40, 45, 50, 55, 59, 64 } },
        { "Bass - 4 strings (E A D G)", "Basse - 4 cordes (Mi La Ré Sol)", { 28, 33, 38, 43 } },
        { "Bass - 5 strings (B E A D G)", "Basse - 5 cordes (Si Mi La Ré Sol)", { 23, 28, 33, 38, 43 } },
        { "Bass - 6 strings (B E A D G C)", "Basse - 6 cordes (Si Mi La Ré Sol Do)", { 23, 28, 33, 38, 43, 48 } },
        { "Bass - 4 strings, drop D", "Basse - 4 cordes, drop D (Ré La Ré Sol)", { 26, 33, 38, 43 } },
        { "Ukulele - soprano (G C E A)", "Ukulélé - soprano (Sol Do Mi La)", { 67, 60, 64, 69 } },
        { "Ukulele - low G (G C E A)", "Ukulélé - sol grave (Sol Do Mi La)", { 55, 60, 64, 69 } },
        { "Ukulele - baritone (D G B E)", "Ukulélé - baryton (Ré Sol Si Mi)", { 50, 55, 59, 64 } },
        { "Violin / mandolin (G D A E)", "Violon / mandoline (Sol Ré La Mi)", { 55, 62, 69, 76 } },
        { "Viola (C G D A)", "Alto (Do Sol Ré La)", { 48, 55, 62, 69 } },
        { "Cello (C G D A)", "Violoncelle (Do Sol Ré La)", { 36, 43, 50, 57 } },
        { "Banjo - open G (g D G B D)", "Banjo - open G (sol Ré Sol Si Ré)", { 67, 50, 55, 59, 62 } },
    };
    return list;
}

int numTunings() noexcept { return static_cast<int> (tunings().size()); }

Match matchFrequency (double hz, double a4, int tuningIndex) noexcept
{
    Match m;
    const double midi = frequencyToMidi (hz, a4);
    if (midi <= 0.0) return m;
    m.valid = true;
    const auto& list = tunings();
    if (tuningIndex <= 0 || tuningIndex >= static_cast<int> (list.size()) || list[static_cast<std::size_t> (tuningIndex)].notes.empty())
    {
        m.midi = static_cast<int> (std::lround (midi));
        m.cents = (midi - m.midi) * 100.0;
        return m;
    }
    const auto& notes = list[static_cast<std::size_t> (tuningIndex)].notes;
    double best = 1.0e9;
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        const double d = std::fabs (midi - notes[i]);
        if (d < best) { best = d; m.stringIndex = static_cast<int> (i); m.midi = notes[i]; }
    }
    m.cents = (midi - m.midi) * 100.0;
    return m;
}

void PitchTracker::prepare (double sampleRate) noexcept
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    factor_ = std::max (1, static_cast<int> (std::lround (sr_ / 24000.0)));
    rate_ = sr_ / factor_;
    count_ = 0;
    acc_ = 0.0f;
    for (auto& v : ring_) v.store (0.0f, std::memory_order_relaxed);
    write_.store (0, std::memory_order_relaxed);
}

void PitchTracker::push (const float* a, const float* b, int n) noexcept
{
    if (a == nullptr || n <= 0) return;
    auto w = write_.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        acc_ += b != nullptr ? 0.5f * (a[i] + b[i]) : a[i];
        if (++count_ >= factor_)
        {
            ring_[w % kRing].store (acc_ / static_cast<float> (factor_), std::memory_order_relaxed);
            ++w;
            acc_ = 0.0f;
            count_ = 0;
        }
    }
    write_.store (w, std::memory_order_release);
}

PitchTracker::Result PitchTracker::analyse() const
{
    Result r;
    const int tauMax = static_cast<int> (rate_ / kMinHz) + 2;
    const int tauMin = std::max (2, static_cast<int> (rate_ / kMaxHz));
    const int window = tauMax + tauMax / 4;            // pas de calcul sur moins de deux périodes du plus grave
    const int total = window + tauMax;
    if (total >= kRing) return r;
    const auto w = write_.load (std::memory_order_acquire);
    if (w < static_cast<std::uint32_t> (total)) return r;
    std::vector<float> x (static_cast<std::size_t> (total));
    for (int i = 0; i < total; ++i) x[static_cast<std::size_t> (i)] = ring_[(w - static_cast<std::uint32_t> (total) + static_cast<std::uint32_t> (i)) % kRing].load (std::memory_order_relaxed);

    // niveau (efficace) de la dernière fenêtre
    double e = 0.0;
    for (int i = total - window; i < total; ++i) e += static_cast<double> (x[static_cast<std::size_t> (i)]) * x[static_cast<std::size_t> (i)];
    const double rms = std::sqrt (e / window);
    r.levelDb = rms > 1.0e-6 ? static_cast<float> (20.0 * std::log10 (rms)) : -120.0f;
    if (r.levelDb < -62.0f) return r;

    // différence normalisée (YIN) ; retire d'abord la composante continue
    double mean = 0.0;
    for (const float v : x) mean += v;
    mean /= total;
    for (auto& v : x) v -= static_cast<float> (mean);
    std::vector<double> d (static_cast<std::size_t> (tauMax) + 1, 0.0);
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double s = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double diff = static_cast<double> (x[static_cast<std::size_t> (j)]) - x[static_cast<std::size_t> (j + tau)];
            s += diff * diff;
        }
        d[static_cast<std::size_t> (tau)] = s;
    }
    std::vector<double> n (d.size(), 1.0);
    double running = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        running += d[static_cast<std::size_t> (tau)];
        n[static_cast<std::size_t> (tau)] = running > 0.0 ? d[static_cast<std::size_t> (tau)] * tau / running : 1.0;
    }
    constexpr double threshold = 0.15;
    int best = -1;
    for (int tau = tauMin; tau < tauMax; ++tau)
        if (n[static_cast<std::size_t> (tau)] < threshold)
        {
            while (tau + 1 < tauMax && n[static_cast<std::size_t> (tau + 1)] < n[static_cast<std::size_t> (tau)]) ++tau;
            best = tau;
            break;
        }
    if (best < 0) return r;

    double tau = best;
    if (best > 1 && best < tauMax)
    {
        const double a = n[static_cast<std::size_t> (best) - 1], b = n[static_cast<std::size_t> (best)], c = n[static_cast<std::size_t> (best) + 1];
        const double den = a - 2.0 * b + c;
        if (std::fabs (den) > 1.0e-12) tau += std::clamp (0.5 * (a - c) / den, -1.0, 1.0);
    }
    r.freq = rate_ / tau;
    r.clarity = static_cast<float> (std::clamp (1.0 - n[static_cast<std::size_t> (best)], 0.0, 1.0));
    r.valid = r.freq >= kMinHz && r.freq <= kMaxHz;
    return r;
}

void ToneGen::render (float* out, int n, bool on, double hz, float amplitude) noexcept
{
    if (n <= 0) return;
    const float step = static_cast<float> (1.0 / (0.01 * sr_));   // fondu de 10 ms
    const double inc = std::clamp (hz, 1.0, 0.45 * sr_) / sr_;
    for (int i = 0; i < n; ++i)
    {
        gain_ += on ? std::min (step, 1.0f - gain_) : -std::min (step, gain_);
        if (gain_ <= 0.0f && ! on) { phase_ = 0.0; continue; }
        out[i] += static_cast<float> (std::sin (6.283185307179586 * phase_)) * gain_ * amplitude;
        phase_ += inc;
        phase_ -= std::floor (phase_);
    }
}
} // namespace neptune::core::tuner
