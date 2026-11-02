#include "neptune/core/Metronome.h"

#include <algorithm>
#include <cmath>

namespace neptune::core::metronome
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;
double dbToLin (double db) noexcept { return db <= -119.0 ? 0.0 : std::pow (10.0, db / 20.0); }
}

void Metronome::prepare (double sampleRate) noexcept
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void Metronome::reset() noexcept
{
    for (auto& v : voices_) v = Voice {};
    haveTick_ = false;
    internalTick_ = 0.0;
    wasActive_ = false;
    beat_.store (-1, std::memory_order_relaxed);
}

void Metronome::trigger (const Settings& s, bool accent, bool sub) noexcept
{
    auto& v = voices_[next_];
    next_ = (next_ + 1) % 3;
    v = Voice {};
    v.active = true;
    double f1 = 1500.0, f2 = 0.0, decay = 0.012, mix2 = 0.0;
    switch (std::clamp (s.sound, 0, kNumSounds - 1))
    {
        case 0: f1 = 1500.0; decay = 0.012; break;                          // clic
        case 1: f1 = 900.0; f2 = 1750.0; mix2 = 0.45; decay = 0.03; break;    // bois
        case 2: f1 = 880.0; decay = 0.07; break;                             // bip
        default: f1 = 587.0; f2 = 845.0; mix2 = 0.8; decay = 0.13; break;     // cloche
    }
    const double pitch = accent ? 1.5 : (sub ? 0.8 : 1.0);
    v.f1 = f1 * pitch;
    v.f2 = f2 * pitch;
    v.decay = decay;
    v.mix2 = mix2;
    double gain = dbToLin (s.levelDb);
    if (accent) gain *= dbToLin (s.accentDb);
    if (sub) gain *= dbToLin (s.subDb);
    v.amp = std::min (gain, 4.0) / (1.0 + mix2);
}

void Metronome::render (const Settings& s, const HostPos& host, float* out, int n) noexcept
{
    if (n <= 0) return;
    std::fill (out, out + n, 0.0f);
    const bool active = s.run && (! s.hostSync || (host.valid && host.playing));
    if (! active)
    {
        if (wasActive_) { for (auto& v : voices_) v.active = false; beat_.store (-1, std::memory_order_relaxed); }
        wasActive_ = false;
        haveTick_ = false;
        internalTick_ = 0.0;
        return;
    }
    const int unit = s.unit == 8 || s.unit == 16 ? s.unit : 4;
    const int sub = std::clamp (s.sub, 1, 4);
    const int beats = std::clamp (s.beats, 1, 12);
    const double bpm = s.hostSync && host.valid && host.bpm > 1.0 ? host.bpm : std::clamp (s.bpm, 20.0, 300.0);
    const double ticksPerQuarter = unit * sub / 4.0;
    const double inc = bpm / 60.0 / sr_ * ticksPerQuarter;   // temps ou subdivisions par échantillon
    double pos0;
    if (s.hostSync) pos0 = host.ppq * ticksPerQuarter;
    else
    {
        if (! wasActive_) { internalTick_ = 0.0; haveTick_ = false; }
        pos0 = internalTick_;
    }
    wasActive_ = true;

    for (int i = 0; i < n; ++i)
    {
        const double pos = pos0 + i * inc;
        const long long idx = static_cast<long long> (std::floor (pos));
        if ((! haveTick_ || idx != lastTick_) && pos - static_cast<double> (idx) <= 2.0 * inc + 1.0e-9)
        {
            // un nouveau temps ou une nouvelle subdivision ; après un saut au milieu d'un temps on attend le suivant
            const long long beatNo = idx >= 0 ? idx / sub : -((-idx + sub - 1) / sub);
            const int subIdx = static_cast<int> (idx - beatNo * sub);
            const int inBar = static_cast<int> (((beatNo % beats) + beats) % beats);
            const bool accent = subIdx == 0 && inBar == 0;
            trigger (s, accent, subIdx != 0);
            if (subIdx == 0)
            {
                beat_.store (inBar, std::memory_order_relaxed);
                clicks_.fetch_add (1, std::memory_order_relaxed);
            }
        }
        if (idx != lastTick_ || ! haveTick_) { lastTick_ = idx; haveTick_ = true; }

        float y = 0.0f;
        for (auto& v : voices_)
        {
            if (! v.active) continue;
            const double t = v.age / sr_;
            const double env = std::min (1.0, t / 0.0008) * std::exp (-t / v.decay);
            if (t > v.decay * 9.0) { v.active = false; continue; }
            double sig = std::sin (kTwoPi * v.phase1);
            if (v.mix2 > 0.0) sig += v.mix2 * std::sin (kTwoPi * v.phase2);
            y += static_cast<float> (sig * env * v.amp);
            v.phase1 += v.f1 / sr_;
            v.phase1 -= std::floor (v.phase1);
            v.phase2 += v.f2 / sr_;
            v.phase2 -= std::floor (v.phase2);
            v.age += 1.0;
        }
        out[i] = y;
    }
    if (! s.hostSync) internalTick_ = pos0 + n * inc;
}
} // namespace neptune::core::metronome
