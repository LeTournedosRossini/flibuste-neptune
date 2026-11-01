#include "neptune/core/LimiterModule.h"

#include <algorithm>
#include <cmath>

namespace neptune::core
{
namespace limiter
{
int lookaheadSamples (double sampleRate, int mode, double lookaheadMs) noexcept
{
    if (mode == kZeroLatency) return 0;
    const double ms = mode == kLowLatency ? lowLatencyMs : std::clamp (lookaheadMs, 1.0, maxLookaheadMs);
    // Low Latency : arrondi vers le bas (52 échantillons à 44,1 kHz, 56 à 48 kHz, comme le limiteur de référence) ; Quality : à l'entier le plus proche (441 / 480)
    const double samples = ms * 0.001 * sampleRate;
    return std::max (2, static_cast<int> (mode == kLowLatency ? std::floor (samples + 1e-9) : std::lround (samples)));
}
} // namespace limiter

LimiterModule::LimiterModule() = default;

bool LimiterModule::declareParams (ParameterSet& set, std::string* error) const
{
    using namespace limiter;
    using P = ParamSpec;
    auto fixed = [] (P s) { s.automatable = false; return s; };
    // deux décimales : l'hôte (et le banc) règle par le texte affiché ; avec une seule, le plafond « 0.0 » couvrirait 0,05 dB
    auto fine = [] (P s) { s.decimals = 2; return s; };
    return set.add (fine (P::continuous (threshold, "Threshold", Unit::Decibel, -60.0, 12.0, -3.0)), error)
        && set.add (fine (P::continuous (ceiling, "Ceiling", Unit::Decibel, -24.0, 0.0, -0.3)), error)
        && set.add (P::continuous (release, "Release", Unit::DecibelPerSecond, 1.0, 100.0, 15.0, Curve::Exponential), error)
        && set.add (fixed (P::continuous (lookahead, "Lookahead", Unit::Milliseconds, 1.0, maxLookaheadMs, maxLookaheadMs)), error)
        && set.add (fixed (P::choice (mode, "Mode", { "Quality", "Low Latency", "Zero Latency" }, kQuality)), error);
}

void LimiterModule::bind (const ParameterSet& set)
{
    for (ParamId id = limiter::threshold; id <= limiter::mode; ++id) idx_[id - limiter::threshold] = set.indexOf (id);
}

void LimiterModule::prepare (double sampleRate, int)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxDelay_ = limiter::lookaheadSamples (sampleRate_, limiter::kQuality, limiter::maxLookaheadMs);
    cap_ = maxDelay_ + 8;
    uL_.assign (static_cast<std::size_t> (cap_), 0.0f);
    uR_.assign (static_cast<std::size_t> (cap_), 0.0f);
    r_.assign (static_cast<std::size_t> (cap_), 1.0f);
    mode_ = delay_ = -1;
    boostDb_ = boostTargetDb_ = 0.0;
    boostLin_ = 1.0;
    reset();
}

int LimiterModule::maxLatencySamples (double sampleRate) const noexcept
{
    return limiter::lookaheadSamples (sampleRate, limiter::kQuality, limiter::maxLookaheadMs) + 1;
}

int LimiterModule::latencySamples (const ParameterSet& params) const noexcept
{
    const int mode = zero_.load (std::memory_order_relaxed) ? static_cast<int> (limiter::kZeroLatency) : std::clamp (static_cast<int> (std::lround (params.getAt (idx_[limiter::mode - limiter::threshold]))), 0, static_cast<int> (limiter::kNumModes) - 1);
    return limiter::lookaheadSamples (sampleRate_, mode, params.getAt (idx_[limiter::lookahead - limiter::threshold]));
}

void LimiterModule::reset() noexcept
{
    std::fill (uL_.begin(), uL_.end(), 0.0f);
    std::fill (uR_.begin(), uR_.end(), 0.0f);
    std::fill (r_.begin(), r_.end(), 1.0f);
    boostSnap_ = true;
    n_ = 0;
    lastBelow_ = -1000000000LL;
    prev_ = 1.0;
    slope_ = slewDefault_;
    holdUntil_ = 0;
    redDb_ = 0.0;
    slowDb_ = 0.0;
    inPeak_[0] = inPeak_[1] = outPeak_[0] = outPeak_[1] = 0.0f;
    lastReductionDb_ = 0.0;
    for (auto& m : meters_) m.store (0.0f, std::memory_order_relaxed);
}

void LimiterModule::configure (int mode, int delay) noexcept
{
    mode_ = mode;
    delay_ = std::min (delay, maxDelay_);
    look_ = mode == limiter::kZeroLatency ? 1 : mode == limiter::kLowLatency ? delay_ : std::max (1, static_cast<int> (std::lround (limiter::rampShare * delay_)));
    rise_ = mode == limiter::kLowLatency ? std::max (1, static_cast<int> (std::lround (limiter::riseMs * 0.001 * sampleRate_)))
                                         : std::max (1, delay_);
    hold_ = mode == limiter::kLowLatency ? static_cast<int> (std::lround (limiter::lowLatencyHoldMs * 0.001 * sampleRate_)) : 0;
    slewDefault_ = 0.93 / rise_;
    reset();
}

void LimiterModule::process (float* left, float* right, int n, const ParameterSet& params) noexcept
{
    using namespace limiter;
    auto get = [&] (ParamId id) { return params.getAt (idx_[id - threshold]); };
    const int mode = zero_.load (std::memory_order_relaxed) ? static_cast<int> (kZeroLatency) : std::clamp (static_cast<int> (std::lround (get (limiter::mode))), 0, static_cast<int> (kNumModes) - 1);   // profil Live : Zero Latency
    const int delay = lookaheadSamples (sampleRate_, mode, get (lookahead));
    if (mode != mode_ || delay != delay_) configure (mode, delay);
    const double ceilDb = get (ceiling);
    ceilLin_ = std::pow (10.0, ceilDb / 20.0);
    boostTargetDb_ = ceilDb - get (threshold);
    if (boostSnap_)   // premier bloc après un démarrage ou un changement de mode : pas de glissement depuis l'ancien gain de repos
    {
        boostDb_ = boostTargetDb_;
        boostLin_ = std::pow (10.0, boostDb_ / 20.0);
        boostSnap_ = false;
    }
    const double rel = get (release);
    slowUp_ = slowUp * rel / sampleRate_;
    slowDown_ = rel / sampleRate_;
    const double boostStep = 1.0 - std::exp (-1.0 / (0.010 * sampleRate_));   // le gain de repos glisse en 10 ms

    float peakIn[2] = { 0, 0 }, peakOut[2] = { 0, 0 };
    for (int i = 0; i < n; ++i)
    {
        if (boostDb_ != boostTargetDb_)
        {
            boostDb_ += (boostTargetDb_ - boostDb_) * boostStep;
            if (std::abs (boostTargetDb_ - boostDb_) < 1e-5) boostDb_ = boostTargetDb_;
            boostLin_ = std::pow (10.0, boostDb_ / 20.0);
        }
        peakIn[0] = std::max (peakIn[0], std::abs (left[i]));
        peakIn[1] = std::max (peakIn[1], std::abs (right[i]));
        float oL, oR;
        step (left[i], right[i], oL, oR);
        left[i] = oL;
        right[i] = oR;
        peakOut[0] = std::max (peakOut[0], std::abs (oL));
        peakOut[1] = std::max (peakOut[1], std::abs (oR));
    }
    for (int c = 0; c < 2; ++c)
    {
        inPeak_[c] = std::max (peakIn[c], inPeak_[c] * 0.9f);
        outPeak_[c] = std::max (peakOut[c], outPeak_[c] * 0.9f);
    }
    meters_[kInPeakL].store (inPeak_[0], std::memory_order_relaxed);
    meters_[kInPeakR].store (inPeak_[1], std::memory_order_relaxed);
    meters_[kOutPeakL].store (outPeak_[0], std::memory_order_relaxed);
    meters_[kOutPeakR].store (outPeak_[1], std::memory_order_relaxed);
    meters_[kReduction].store (static_cast<float> (lastReductionDb_), std::memory_order_relaxed);
}

// Un échantillon d'entrée : on range u et r, puis on calcule le gain de l'échantillon d'il y a `delay_` pas (dont les `delay_` suivants sont connus).
void LimiterModule::step (float inL, float inR, float& outL, float& outR) noexcept
{
    if (mode_ == limiter::kZeroLatency)
    {
        // aucune anticipation : le gain tombe d'un coup à l'échantillon qui dépasse le plafond, puis remonte de Release dB par seconde
        const float zL = static_cast<float> (inL * boostLin_), zR = static_cast<float> (inR * boostLin_);
        const double zPeak = std::max (std::abs (static_cast<double> (zL)), std::abs (static_cast<double> (zR)));
        const double zr = std::min (1.0, ceilLin_ / std::max (zPeak, 1e-12));
        const double needDb = zr < 1.0 ? -20.0 * std::log10 (zr) : 0.0;
        const double decayed = redDb_ - slowDown_;
        double zg;
        if (needDb >= decayed) { redDb_ = needDb; zg = zr; }   // le plafond est tenu exactement sur cet échantillon
        else { redDb_ = std::max (decayed, 0.0); zg = redDb_ > 0.0 ? std::pow (10.0, -redDb_ / 20.0) : 1.0; }
        lastReductionDb_ = redDb_;
        outL = static_cast<float> (zL * zg);
        outR = static_cast<float> (zR * zg);
        return;
    }
    const auto cap = static_cast<std::int64_t> (cap_);
    const auto slot = [cap] (std::int64_t t) { return static_cast<std::size_t> (((t % cap) + cap) % cap); };

    const float uL = static_cast<float> (inL * boostLin_), uR = static_cast<float> (inR * boostLin_);
    const double peak = std::max (std::abs (static_cast<double> (uL)), std::abs (static_cast<double> (uR)));
    const double r = std::min (1.0, ceilLin_ / std::max (peak, 1e-12));
    const std::size_t w = slot (n_);
    uL_[w] = uL;
    uR_[w] = uR;
    r_[w] = static_cast<float> (r);
    if (r < 0.999999) lastBelow_ = n_;

    const std::int64_t t = n_ - delay_;   // instant dont on calcule le gain
    ++n_;
    auto rAt = [&] (std::int64_t k) -> double { return r_[slot (t + k)]; };

    // ---- étage rapide
    double f;
    const bool lowLatency = mode_ == limiter::kLowLatency;
    if (prev_ >= 1.0 - 1e-12)
    {
        f = 1.0;
        if (lastBelow_ >= t)
        {
            double best = 1.0;
            int bestK = 0;
            for (int k = 0; k <= look_; ++k)
            {
                const double cand = 1.0 - (1.0 - rAt (k)) * (1.0 - static_cast<double> (k) / look_);
                if (cand < best) { best = cand; bestK = k; }
            }
            f = std::min (1.0, best);
            holdUntil_ = t + bestK + hold_;   // le maintien se compte depuis le pic, pas depuis le début de la descente
        }
        prev_ = f;
    }
    else
    {
        const bool holding = lowLatency && t < holdUntil_;
        double candRel = holding ? prev_ : std::min (1.0, prev_ + slope_);
        if (lastBelow_ >= t)
        {
            const int window = delay_;   // portée de la vision vers l'avant (en pas)
            double allowedMin = 2.0;
            int allowedK = 1;
            for (int k = 1; k <= window; ++k)
            {
                const double wj = rAt (k - 1);
                if (wj < candRel)
                {
                    const double allowed = prev_ > wj ? prev_ + (wj - prev_) / k : wj;
                    if (allowed < allowedMin) { allowedMin = allowed; allowedK = k; }
                }
            }
            if (allowedMin < candRel)
            {
                candRel = allowedMin;
                slope_ = (1.0 - candRel) / rise_;
                if (lowLatency && (! holding || allowedMin < prev_ * 0.9999)) holdUntil_ = t + allowedK - 1 + hold_;   // le maintien repart d'un pic arrivé après sa fin (même si la descente est minuscule : Q500) ou d'un pic plus profond que le creux tenu (Za9)
            }
        }
        prev_ = candRel;
        f = prev_;
        if (prev_ >= 1.0 - 1e-12) holdUntil_ = 0;
    }

    // ---- étage lent (dB)
    const double wanted = f < 1.0 ? -20.0 * std::log10 (std::max (f, 1e-9)) : 0.0;
    if (wanted > slowDb_) slowDb_ = std::min (slowDb_ + slowUp_, wanted);
    else slowDb_ = std::max (slowDb_ - slowDown_, 0.0);
    double g = f;
    if (slowDb_ > 0.0) g = std::min (f, std::pow (10.0, -slowDb_ / 20.0));
    lastReductionDb_ = g < 1.0 ? -20.0 * std::log10 (g) : 0.0;

    const std::size_t rd = slot (t);
    outL = static_cast<float> (uL_[rd] * g);
    outR = static_cast<float> (uR_[rd] * g);
}
} // namespace neptune::core
