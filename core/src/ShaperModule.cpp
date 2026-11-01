#include "neptune/core/ShaperModule.h"

#include <algorithm>
#include <cmath>

namespace neptune::core
{
using namespace shaper;

namespace
{
constexpr double kPi = 3.14159265358979323846;
inline double dbToLin (double db) noexcept { return std::exp (db * 0.11512925464970229); }
inline double linToDb (double lin) noexcept { return 20.0 * std::log10 (std::max (lin, 1.0e-9)); }
inline double coefFor (double ms, double fs) noexcept { return 1.0 - std::exp (-1.0 / (std::max (ms, 0.01) * 0.001 * fs)); }
inline double smoothstep (double t) noexcept { t = std::clamp (t, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }

ParamSpec stepsOf (ParamId id, const char* name, int lo, int hi, int def)
{
    auto s = ParamSpec::continuous (id, name, Unit::None, lo, hi, def);
    return s;
}
} // namespace

ShaperModule::ShaperModule()
{
    for (auto& m : meters_) m.store (0.0f, std::memory_order_relaxed);
    keyRing_.assign (static_cast<std::size_t> (kKeyRing), 0.0f);
}

bool ShaperModule::declareParams (ParameterSet& set, std::string* error) const
{
    using P = ParamSpec;
    auto pct = [] (ParamId id, const char* name, double lo, double hi, double def) { return P::continuous (id, name, Unit::Percent, lo, hi, def); };
    auto db = [] (ParamId id, const char* name, double lo, double hi, double def) { return P::continuous (id, name, Unit::Decibel, lo, hi, def); };
    auto fixed = [] (P s) { s.automatable = false; return s; };
    bool ok = true;
    auto add = [&] (P s) { ok = ok && set.add (std::move (s), error); };

    add (pct (attackGain1, "Attack 1", -100.0, 100.0, 0.0));
    add (pct (attackGain2, "Attack 2", -100.0, 100.0, 0.0));
    add (pct (sustainGain1, "Sustain 1", -100.0, 100.0, 0.0));
    add (pct (sustainGain2, "Sustain 2", -100.0, 100.0, 0.0));
    add (pct (levelDependency, "Level", -100.0, 100.0, 0.0));
    add (db (levelThreshold, "Threshold", -60.0, 0.0, -24.0));
    add (db (levelKnee, "Knee", 0.0, 24.0, 6.0));
    add (P::continuous (attackTime, "Attack Time", Unit::Milliseconds, 0.1, 100.0, 10.0, Curve::Exponential));
    add (P::continuous (sustainTime, "Sustain Time", Unit::Milliseconds, 10.0, 1000.0, 150.0, Curve::Exponential));
    add (pct (shapeSmoothing, "Shape Smoothing", -50.0, 50.0, 0.0));
    add (db (range, "Range", 6.0, 24.0, 15.0));
    add (pct (stereoLink, "Link", 0.0, 100.0, 100.0));
    add (fixed (P::choice (channelMode, "Channel Mode", { "Stereo", "Dual Mono", "M-S" }, 0)));
    add (P::toggle (delta, "Delta"));
    add (fixed (P::continuous (lookahead, "Lookahead", Unit::Milliseconds, 0.0, maxLookaheadMs, 0.0)));
    add (fixed (P::choice (oversampling, "Oversampling", { "None", "2x", "4x" }, 0)));
    add (fixed (db (noiseFloor, "Noise Floor", -100.0, -40.0, -70.0)));
    add (fixed (P::choice (detMode, "Detection", { "Peak", "RMS" }, 0)));
    add (fixed (P::continuous (detFastRatio, "Fast / Slow Ratio", Unit::Ratio, 3.0, 30.0, 10.0, Curve::Exponential)));

    add (P::toggle (scEnable, "Sidechain Enable"));
    add (fixed (P::choice (scSource, "Sidechain Source", { "Internal", "External", "Mix" }, 0)));
    add (db (scGain, "Sidechain Gain", -24.0, 24.0, 0.0));
    add (db (scIntVol, "Sidechain Comp Vol", -24.0, 24.0, 0.0));
    add (db (scExtVol, "Sidechain Ext Vol", -24.0, 24.0, 0.0));
    add (P::toggle (scPhaseInt, "Sidechain Phase Comp"));
    add (P::toggle (scPhaseExt, "Sidechain Phase Ext"));
    add (fixed (P::toggle (scExtMs, "Sidechain Ext M/S")));
    add (P::toggle (scListen, "Sidechain Listen"));
    add (P::toggle (scEqOn, "Sidechain EQ", true));
    add (fixed (P::toggle (scAnalyzerOn, "Sidechain Analyzer", true)));
    add (P::continuous (scHpFreq, "Sidechain HP Freq", Unit::Hertz, 20.0, 2000.0, 80.0, Curve::Exponential));
    add (fixed (stepsOf (scHpSlope, "Sidechain HP Slope", 6, 48, 12)));
    add (P::continuous (scLpFreq, "Sidechain LP Freq", Unit::Hertz, 1000.0, 20000.0, 16000.0, Curve::Exponential));
    add (fixed (stepsOf (scLpSlope, "Sidechain LP Slope", 6, 48, 12)));
    const double bandDefault[3] = { 250.0, 1200.0, 5000.0 };
    const ParamId first[3] = { scBand1Freq, scBand2Freq, scBand3Freq };
    for (int b = 0; b < 3; ++b)
    {
        const std::string n = "Sidechain Band " + std::to_string (b + 1);
        add (P::continuous (first[b], n + " Freq", Unit::Hertz, 20.0, 20000.0, bandDefault[b], Curve::Exponential));
        add (db (first[b] + 1, (n + " Gain").c_str(), -18.0, 18.0, 0.0));
        add (P::continuous (first[b] + 2, n + " Q", Unit::None, 0.3, 12.0, 1.0, Curve::Exponential));
    }
    add (fixed (P::continuous (meterRmsMs, "Meter RMS Window", Unit::Milliseconds, 10.0, 3000.0, 300.0, Curve::Exponential)));
    add (fixed (P::continuous (meterPeakDecay, "Meter Peak Decay", Unit::None, 5.0, 300.0, 40.0)));
    {
        auto hid = [] (P p) { p.hidden = true; p.automatable = false; p.modulatable = false; return p; };
        add (hid (P::choice (anTime, "Analyzer Time", { "6 s", "9 s", "12 s", "18 s" }, 3)));
        add (hid (P::choice (anMove, "Analyzer Move", { "Sync", "Slow", "Roll" }, 0)));
    }
    return ok;
}

void ShaperModule::bind (const ParameterSet& set)
{
    for (std::size_t i = 0; i < idx_.size(); ++i) idx_[i] = set.indexOf (static_cast<ParamId> (100 + i));
}

int ShaperModule::latencySamples (const ParameterSet& params) const noexcept
{
    const auto at = [&] (ParamId id) { return idx_[static_cast<std::size_t> (id - 100)] == ParameterSet::npos ? 0.0 : params.getAt (idx_[static_cast<std::size_t> (id - 100)]); };
    const bool zeroLat = zero_.load (std::memory_order_relaxed);
    const int look = zeroLat ? 0 : static_cast<int> (std::lround (std::clamp (at (lookahead), 0.0, maxLookaheadMs) * 0.001 * sampleRate_));
    const int os = zeroLat ? 0 : static_cast<int> (std::lround (at (oversampling)));
    return look + Oversampler::latencyFor (os >= 2 ? 4 : os == 1 ? 2 : 1);
}

int ShaperModule::maxLatencySamples (double sampleRate) const noexcept
{
    return static_cast<int> (std::ceil (maxLookaheadMs * 0.001 * sampleRate)) + Oversampler::kLatency4x + 2;
}

void ShaperModule::prepare (double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max (1, maxBlockSize);
    const auto n = static_cast<std::size_t> (maxBlock_);
    for (auto* v : { &in0_, &in1_, &key0_, &key1_, &work0_, &work1_, &gain0_, &gain1_ }) v->assign (n, 0.0f);
    for (auto& v : anBuf_) v.assign (n, 0.0f);
    analyzer_.setSampleRate (sampleRate_);
    analyzer_.setMaxBlock (maxBlock_);
    hiGain_.assign (4 * n, 0.0f);
    const auto lookMax = static_cast<std::size_t> (std::ceil (maxLookaheadMs * 0.001 * sampleRate_)) + 2;
    const auto dryMax = static_cast<std::size_t> (maxLatencySamples (sampleRate_)) + 2;
    for (auto& c : ch_)
    {
        c.os.prepare (maxBlock_);
        c.delay.assign (lookMax, 0.0f);
        c.dryDelay.assign (dryMax, 0.0f);
        c.gainRing.assign (static_cast<std::size_t> (Oversampler::upDelayHigh (4)) + 2, 1.0f);
    }
    reset();
}

void ShaperModule::reset() noexcept
{
    for (auto& c : ch_)
    {
        c.os.reset();
        std::fill (c.delay.begin(), c.delay.end(), 0.0f);
        std::fill (c.dryDelay.begin(), c.dryDelay.end(), 0.0f);
        std::fill (c.gainRing.begin(), c.gainRing.end(), 1.0f);
        c.delayPos = c.dryPos = c.gainPos = 0;
        c.hp.reset();
        c.lp.reset();
        for (auto& b : c.band) b.reset();
        c.fastEnv = c.slowEnv = 0.0;
        c.gainDb = 0.0;
        c.lastGainLin = 1.0;
        c.atkG = c.susG = 0.0f;
    }
    designed_ = false;
    for (auto& m : meters_) m.store (0.0f, std::memory_order_relaxed);
}

void ShaperModule::readParams (const ParameterSet& params) noexcept
{
    for (std::size_t i = 0; i < idx_.size(); ++i)
        if (idx_[i] != ParameterSet::npos) values_[i] = params.getAt (idx_[i]);
}

void ShaperModule::designKeyFilters (const ParameterSet&) noexcept
{
    const double d[20] = { param (scHpFreq), param (scHpSlope), param (scLpFreq), param (scLpSlope), param (scBand1Freq), param (scBand1Gain), param (scBand1Q),
                           param (scBand2Freq), param (scBand2Gain), param (scBand2Q), param (scBand3Freq), param (scBand3Gain), param (scBand3Q), sampleRate_,
                           0, 0, 0, 0, 0, 0 };
    bool same = designed_;
    for (int i = 0; i < 14 && same; ++i) same = d[i] == lastDesign_[static_cast<std::size_t> (i)];
    if (same) return;
    std::copy (std::begin (d), std::end (d), lastDesign_.begin());
    designed_ = true;
    const int hpSlope = static_cast<int> (std::lround (std::clamp (param (scHpSlope), 6.0, 48.0) / 6.0)) * 6;
    const int lpSlope = static_cast<int> (std::lround (std::clamp (param (scLpSlope), 6.0, 48.0) / 6.0)) * 6;
    for (auto& c : ch_)
    {
        c.hp.design (true, sampleRate_, param (scHpFreq), hpSlope);
        c.lp.design (false, sampleRate_, param (scLpFreq), lpSlope);
        c.band[0].set (BiquadCoefs::peaking (sampleRate_, param (scBand1Freq), param (scBand1Gain), param (scBand1Q)));
        c.band[1].set (BiquadCoefs::peaking (sampleRate_, param (scBand2Freq), param (scBand2Gain), param (scBand2Q)));
        c.band[2].set (BiquadCoefs::peaking (sampleRate_, param (scBand3Freq), param (scBand3Gain), param (scBand3Q)));
    }
}

double ShaperModule::detectionResponseDb (const ParameterSet& params, double f) const noexcept
{
    auto at = [&] (ParamId id) { return idx_[static_cast<std::size_t> (id - 100)] == ParameterSet::npos ? 0.0 : params.getAt (idx_[static_cast<std::size_t> (id - 100)]); };
    const double fs = sampleRate_;
    ButterworthCascade hp, lp;
    hp.design (true, fs, at (scHpFreq), static_cast<int> (std::lround (std::clamp (at (scHpSlope), 6.0, 48.0) / 6.0)) * 6);
    lp.design (false, fs, at (scLpFreq), static_cast<int> (std::lround (std::clamp (at (scLpSlope), 6.0, 48.0) / 6.0)) * 6);
    double m = hp.magnitude (fs, f) * lp.magnitude (fs, f);
    const ParamId first[3] = { scBand1Freq, scBand2Freq, scBand3Freq };
    for (const auto b : first) m *= BiquadCoefs::peaking (fs, at (b), at (b + 1), at (b + 2)).magnitude (fs, f);
    return 20.0 * std::log10 (std::max (m, 1.0e-9));
}

void ShaperModule::copyKey (float* dst, int n) const noexcept
{
    n = std::min (n, kKeyRing);
    const auto head = keyHead_.load (std::memory_order_acquire);
    for (int i = 0; i < n; ++i) dst[i] = keyRing_[(head + static_cast<std::uint32_t> (kKeyRing - n + i)) % static_cast<std::uint32_t> (kKeyRing)];
}

void ShaperModule::process (float* left, float* right, int n, const ParameterSet& params) noexcept
{
    if (n <= 0 || n > maxBlock_) return;
    readParams (params);
    designKeyFilters (params);
    const double fs = sampleRate_;
    const auto un = static_cast<std::size_t> (n);

    // ---- réglages du bloc
    const int mode = static_cast<int> (std::lround (param (channelMode)));            // 0 Stereo, 1 Dual Mono, 2 M-S
    const bool zeroLat = zero_.load (std::memory_order_relaxed);   // profil Live : ni suréchantillonnage ni anticipation
    const int osIndex = zeroLat ? 0 : static_cast<int> (std::lround (param (oversampling)));
    const int factor = osIndex >= 2 ? 4 : osIndex == 1 ? 2 : 1;
    if (factor != factor_)
    {
        factor_ = factor;
        for (auto& c : ch_) { c.os.setFactor (factor); std::fill (c.gainRing.begin(), c.gainRing.end(), 1.0f); }
    }
    for (auto& c : ch_) c.os.setFactor (factor_);
    const int look = zeroLat ? 0 : std::min (static_cast<int> (std::lround (std::clamp (param (lookahead), 0.0, maxLookaheadMs) * 0.001 * fs)), static_cast<int> (ch_[0].delay.size()) - 2);
    const int totalLatency = look + Oversampler::latencyFor (factor_);
    const bool scOn = param (scEnable) >= 0.5;
    const int source = scOn ? static_cast<int> (std::lround (param (scSource))) : 0;
    const bool haveKey = keyL_ != nullptr && keyR_ != nullptr;
    const bool useExt = (source == 1 || source == 2) && haveKey;
    const double keyGain = dbToLin (param (scGain));
    const double intVol = dbToLin (param (scIntVol)) * (param (scPhaseInt) >= 0.5 ? -1.0 : 1.0);
    const double extVol = dbToLin (param (scExtVol)) * (param (scPhaseExt) >= 0.5 ? -1.0 : 1.0);
    const bool extIsMs = param (scExtMs) >= 0.5;
    const bool eqOn = scOn && param (scEqOn) >= 0.5;
    const bool listen = scOn && param (scListen) >= 0.5;
    const bool rms = param (detMode) >= 0.5;
    const double atk = param (attackTime), sus = param (sustainTime), ratio = param (detFastRatio);
    const double slowAtkC = coefFor (atk, fs), slowRelC = coefFor (sus, fs);
    const double fastAtkC = coefFor (std::max (0.05, atk / ratio), fs), fastRelC = coefFor (sus / ratio, fs);
    const double smoothAuto = std::min (50.0, std::max (1.0, 0.3 * std::min (atk, sus)));
    const double smoothC = coefFor (smoothAuto * std::pow (2.0, param (shapeSmoothing) / 50.0), fs);
    const double floorDb = param (noiseFloor);
    const double ctlC = coefFor (15.0, fs);
    const bool deltaOn = param (delta) >= 0.5;
    const double tgtLevel = param (levelDependency) / 100.0, tgtThr = param (levelThreshold), tgtKnee = param (levelKnee), tgtRange = param (range), tgtLink = param (stereoLink) / 100.0;
    const double atkG[2] = { param (attackGain1) / 100.0, mode == 0 ? param (attackGain1) / 100.0 : param (attackGain2) / 100.0 };
    const double susG[2] = { param (sustainGain1) / 100.0, mode == 0 ? param (sustainGain1) / 100.0 : param (sustainGain2) / 100.0 };

    const bool analyse = analyzer_.active();
    // ---- entrée : mesures, passage dans le domaine des canaux (L/R ou M/S)
    if (analyse)
    {
        std::copy (left, left + n, anBuf_[2 * kAnIn].begin());
        std::copy (right, right + n, anBuf_[2 * kAnIn + 1].begin());
    }
    for (int i = 0; i < n; ++i)
    {
        const float l = left[i], r = right[i];
        peakHold_[0] = std::max (peakHold_[0], std::fabs (l));
        peakHold_[1] = std::max (peakHold_[1], std::fabs (r));
        rmsSum_[0] += static_cast<double> (l) * l;
        rmsSum_[1] += static_cast<double> (r) * r;
        if (mode == 2) { in0_[static_cast<std::size_t> (i)] = 0.5f * (l + r); in1_[static_cast<std::size_t> (i)] = 0.5f * (l - r); }
        else { in0_[static_cast<std::size_t> (i)] = l; in1_[static_cast<std::size_t> (i)] = r; }
    }

    // ---- clé, suiveurs, gain (cadence de base)
    float* const key[2] = { key0_.data(), key1_.data() };
    float* const gainLin[2] = { gain0_.data(), gain1_.data() };
    float* const in[2] = { in0_.data(), in1_.data() };
    for (int i = 0; i < n; ++i)
    {
        sLevel_ += ctlC * (tgtLevel - sLevel_);
        sThr_ += ctlC * (tgtThr - sThr_);
        sKnee_ += ctlC * (tgtKnee - sKnee_);
        sRange_ += ctlC * (tgtRange - sRange_);
        sLink_ += ctlC * (tgtLink - sLink_);

        double k[2];
        double ext[2] = { 0.0, 0.0 };
        if (useExt)
        {
            const double el = keyL_[i], er = keyR_[i];
            if (extIsMs || mode != 2) { ext[0] = el; ext[1] = er; }
            else { ext[0] = 0.5 * (el + er); ext[1] = 0.5 * (el - er); }
        }
        for (int c = 0; c < 2; ++c)
        {
            const double x = in[c][i];
            double v = x;
            if (scOn)
            {
                if (source == 0 || ! useExt) v = x * keyGain;
                else if (source == 1) v = ext[c] * keyGain;
                else v = x * intVol + ext[c] * extVol;
            }
            if (eqOn)
            {
                auto& ch = ch_[static_cast<std::size_t> (c)];
                v = ch.hp.process (static_cast<float> (v));
                v = ch.band[0].process (static_cast<float> (v));
                v = ch.band[1].process (static_cast<float> (v));
                v = ch.band[2].process (static_cast<float> (v));
                v = ch.lp.process (static_cast<float> (v));
            }
            k[c] = v;
            key[c][i] = static_cast<float> (v);
        }
        // détection : en Stereo, la part commune est réglée par LINK
        double mag[2] = { rms ? k[0] * k[0] : std::fabs (k[0]), rms ? k[1] * k[1] : std::fabs (k[1]) };
        if (mode == 0)
        {
            const double common = 0.5 * (mag[0] + mag[1]);
            mag[0] = sLink_ * common + (1.0 - sLink_) * mag[0];
            mag[1] = sLink_ * common + (1.0 - sLink_) * mag[1];
        }
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = ch_[static_cast<std::size_t> (c)];
            ch.fastEnv += (mag[c] > ch.fastEnv ? fastAtkC : fastRelC) * (mag[c] - ch.fastEnv);
            ch.slowEnv += (mag[c] > ch.slowEnv ? slowAtkC : slowRelC) * (mag[c] - ch.slowEnv);
            const double fastDb = rms ? 10.0 * std::log10 (std::max (ch.fastEnv, 1.0e-18)) : linToDb (ch.fastEnv);
            const double slowDb = rms ? 10.0 * std::log10 (std::max (ch.slowEnv, 1.0e-18)) : linToDb (ch.slowEnv);
            const double d = fastDb - slowDb;
            const double aPrime = std::clamp (std::max (d, 0.0) / 12.0, 0.0, 1.0), sPrime = std::clamp (std::max (-d, 0.0) / 12.0, 0.0, 1.0);
            const double t = sKnee_ < 0.01 ? (fastDb > sThr_ ? 1.0 : 0.0) : smoothstep (0.5 + (fastDb - sThr_) / sKnee_);
            const double lv = std::fabs (sLevel_);
            const double w = sLevel_ >= 0.0 ? (1.0 - lv) + lv * t : (1.0 - lv) + lv * (1.0 - t);
            double target = sRange_ * (atkG[c] * aPrime + susG[c] * sPrime) * w;
            if (fastDb < floorDb) target = 0.0;
            ch.gainDb += smoothC * (target - ch.gainDb);
            gainLin[c][i] = static_cast<float> (dbToLin (ch.gainDb));
            if (analyse)
            {
                anBuf_[static_cast<std::size_t> (2 * kAnFast + c)][static_cast<std::size_t> (i)] = static_cast<float> (rms ? std::sqrt (ch.fastEnv) : ch.fastEnv);
                anBuf_[static_cast<std::size_t> (2 * kAnSlow + c)][static_cast<std::size_t> (i)] = static_cast<float> (rms ? std::sqrt (ch.slowEnv) : ch.slowEnv);
                anBuf_[static_cast<std::size_t> (2 * kAnGain + c)][static_cast<std::size_t> (i)] = static_cast<float> (ch.gainDb);
            }
        }
        // clé affichée (analyseur de spectre)
        const float kv = static_cast<float> (0.5 * (k[0] + k[1]));
        keyRing_[keyHead_.load (std::memory_order_relaxed) % static_cast<std::uint32_t> (kKeyRing)] = kv;
        keyHead_.store (keyHead_.load (std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // ---- signal : lookahead, suréchantillonnage, gain retardé, retour à la cadence de base
    float* const work[2] = { work0_.data(), work1_.data() };
    for (int c = 0; c < 2; ++c)
    {
        auto& ch = ch_[static_cast<std::size_t> (c)];
        // sec (pour DELTA), retardé de la latence totale
        const auto dsz = ch.dryDelay.size();
        // lookahead
        const auto lsz = ch.delay.size();
        for (int i = 0; i < n; ++i)
        {
            const std::size_t w = (ch.delayPos + static_cast<std::size_t> (i)) % lsz;
            ch.delay[w] = in[c][i];
            work[c][i] = look > 0 ? ch.delay[(w + lsz - static_cast<std::size_t> (look)) % lsz] : in[c][i];
        }
        ch.delayPos = (ch.delayPos + un) % lsz;
        float* hi = ch.os.up (work[c], n);
        const int D = Oversampler::upDelayHigh (factor_);
        const auto rsz = ch.gainRing.size();
        const int F = factor_;
        double prev = ch.lastGainLin;
        for (int i = 0; i < n; ++i)
        {
            const double g = gainLin[c][i];
            for (int f = 0; f < F; ++f)
            {
                const double gi = prev + (g - prev) * static_cast<double> (f + 1) / static_cast<double> (F);
                ch.gainRing[ch.gainPos] = static_cast<float> (gi);
                const std::size_t rd = (ch.gainPos + rsz - static_cast<std::size_t> (D)) % rsz;
                hi[i * F + f] *= ch.gainRing[rd];
                ch.gainPos = (ch.gainPos + 1) % rsz;
            }
            prev = g;
        }
        ch.lastGainLin = prev;
        ch.os.down (work[c], n);   // `work` contient maintenant le signal traité à la cadence de base, retardé de la latence totale
        // signal sec aligné (DELTA) : l'entrée retardée de la latence totale
        for (int i = 0; i < n; ++i)
        {
            const std::size_t w = (ch.dryPos + static_cast<std::size_t> (i)) % dsz;
            ch.dryDelay[w] = in[c][i];
            if (deltaOn) work[c][i] -= ch.dryDelay[(w + dsz - static_cast<std::size_t> (totalLatency)) % dsz];
        }
        ch.dryPos = (ch.dryPos + un) % dsz;
    }

    // ---- sortie : écoute de la clé, retour L/R, mesures
    for (int i = 0; i < n; ++i)
    {
        float a = listen ? key0_[static_cast<std::size_t> (i)] : work0_[static_cast<std::size_t> (i)];
        float b = listen ? key1_[static_cast<std::size_t> (i)] : work1_[static_cast<std::size_t> (i)];
        if (mode == 2) { const float l = a + b, r = a - b; a = l; b = r; }
        left[i] = a;
        right[i] = b;
        peakHold_[2] = std::max (peakHold_[2], std::fabs (a));
        peakHold_[3] = std::max (peakHold_[3], std::fabs (b));
        rmsSum_[2] += static_cast<double> (a) * a;
        rmsSum_[3] += static_cast<double> (b) * b;
    }
    if (analyse)
    {
        std::copy (left, left + n, anBuf_[2 * kAnOut].begin());
        std::copy (right, right + n, anBuf_[2 * kAnOut + 1].begin());
        const float* streams[2 * kAnStreams];
        for (std::size_t k = 0; k < anBuf_.size(); ++k) streams[k] = anBuf_[k].data();
        analyzer_.push (streams, n);
    }
    rmsCount_ += n;
    const int rmsWindow = std::max (1, static_cast<int> (param (meterRmsMs) * 0.001 * fs));
    meters_[kInPeakL].store (peakHold_[0], std::memory_order_relaxed);
    meters_[kInPeakR].store (peakHold_[1], std::memory_order_relaxed);
    meters_[kOutPeakL].store (peakHold_[2], std::memory_order_relaxed);
    meters_[kOutPeakR].store (peakHold_[3], std::memory_order_relaxed);
    for (int c = 0; c < 4; ++c) peakHold_[c] = 0.0f;
    if (rmsCount_ >= rmsWindow || rmsCount_ >= static_cast<int> (fs * 0.1))
    {
        for (int c = 0; c < 4; ++c)
        {
            meters_[static_cast<std::size_t> (kInRmsL + c)].store (static_cast<float> (std::sqrt (rmsSum_[c] / rmsCount_)), std::memory_order_relaxed);
            rmsSum_[c] = 0.0;
        }
        rmsCount_ = 0;
    }
    meters_[kGainL].store (static_cast<float> (ch_[0].gainDb), std::memory_order_relaxed);
    meters_[kGainR].store (static_cast<float> (ch_[1].gainDb), std::memory_order_relaxed);
}
} // namespace neptune::core
