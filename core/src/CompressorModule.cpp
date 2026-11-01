// Compresseur : voir CompressorModule.h (porté de ZL Compressor, zsliu98, AGPLv3).
#include "neptune/core/CompressorModule.h"

#include <algorithm>
#include <cmath>

namespace neptune::core
{
using namespace compressor;

namespace
{
constexpr double kLn10 = 2.30258509299404568402;
}

CompressorModule::CompressorModule()
{
    for (auto& m : meters_) m.store (0.0f, std::memory_order_relaxed);
}

bool CompressorModule::declareParams (ParameterSet& set, std::string* error) const
{
    using P = ParamSpec;
    bool ok = true;
    auto add = [&] (P s) { ok = ok && set.add (std::move (s), error); };
    auto fixed = [] (P s) { s.automatable = false; return s; };
    auto db = [] (ParamId id, const char* n, double lo, double hi, double def) { return P::continuous (id, n, Unit::Decibel, lo, hi, def); };
    auto pct = [] (ParamId id, const char* n, double lo, double hi, double def) { return P::continuous (id, n, Unit::Percent, lo, hi, def); };

    add (P::choice (style, "Compressor Style", { "Clean", "Classic", "Optical", "Vocal" }, 0));
    add (P::choice (direction, "Compression Direction", { "Compress", "Inflate", "Expand", "Shape" }, 0));
    add (db (threshold, "Threshold", -100.0, 0.0, -32.0));
    add (P::continuous (ratio, "Ratio", Unit::Ratio, 1.0, 100.0, 3.0, Curve::Exponential));
    add (P::continuous (kneeWidth, "Knee Width", Unit::Decibel, 0.0, 32.0, 8.0, Curve::Power, 2.0));
    add (pct (curve, "Curve", 0.0, 100.0, 50.0));
    add (db (floorDb, "Floor", -200.0, -80.0, -140.0));
    add (P::continuous (attack, "Attack", Unit::Milliseconds, 0.0, 1000.0, 100.0, Curve::Power, 2.0));
    add (P::continuous (release, "Release", Unit::Milliseconds, 0.0, 5000.0, 500.0, Curve::Power, 2.0));
    add (pct (pump, "Pump / Punch", -100.0, 100.0, 0.0));
    add (pct (smooth, "Smooth", 0.0, 100.0, 0.0));
    add (db (range, "Range", 0.0, 80.0, 80.0));
    add (P::toggle (rangeInf, "Range INF"));
    add (P::continuous (hold, "Hold", Unit::Milliseconds, 0.0, 1000.0, 0.0, Curve::Power, 2.0));
    add (db (makeup, "Makeup Gain", -30.0, 30.0, 0.0));
    add (pct (wet, "Wet", 0.0, 100.0, 100.0));
    add (P::choice (externalSide, "External Side", { "Internal", "External", "MIDI" }, 0));   // 0 et 1 comme l'ancien interrupteur
    add (P::toggle (sideOut, "Side Out"));
    add (P::toggle (sideEqBypass, "Side EQ Bypass", true));   // éteint par défaut : l'interface le rallume avec un jeu de filtres de départ
    add (db (sideGain, "Side Gain", -15.0, 15.0, 0.0));
    add (P::choice (stereoMode, "Side Stereo Mode", { "Mid Side", "Left Right", "Mid Side Max", "Left Right Max" }, 0));
    add (P::toggle (stereoSwap, "Side Stereo Swap"));
    add (pct (stereoLink, "Side Stereo Link", 0.0, 100.0, 0.0));
    add (pct (wet1, "Side Stereo Wet 1", 0.0, 100.0, 100.0));
    add (pct (wet2, "Side Stereo Wet 2", 0.0, 100.0, 100.0));
    add (P::toggle (delta, "Compressor Delta"));
    add (pct (clipperDrive, "Clipper Drive", 0.0, 100.0, 0.0));
    add (fixed (P::choice (oversample, "Oversample", { "Off", "2x", "4x" }, 0)));
    add (fixed (P::continuous (lookahead, "Lookahead", Unit::Milliseconds, -maxLookaheadMs, maxLookaheadMs, 0.0)));
    {
        auto hid = [] (P p) { p.hidden = true; p.automatable = false; p.modulatable = false; return p; };
        add (hid (P::choice (anTime, "Analyzer Time", { "6 s", "9 s", "12 s", "18 s" }, 3)));
        add (hid (P::choice (anMove, "Analyzer Move", { "Sync", "Slow", "Roll" }, 0)));
        add (hid (P::choice (anMaxDb, "Analyzer Max dB", { "6", "0", "-6", "-12", "-24" }, 1)));
        add (hid (P::choice (anMinDb, "Analyzer Min dB", { "-9", "-18", "-36", "-54", "-72", "-120" }, 3)));
        add (hid (P::choice (anMag, "Analyzer Magnitude", { "Peak", "RMS" }, 0)));
        add (hid (P::choice (anStereo, "Analyzer Channels", { "M+S", "L+R", "Left", "Right", "Mid", "Side" }, 0)));
        add (hid (P::toggle (anShowIn, "Analyzer Show Input", true)));
        add (hid (P::toggle (anShowOut, "Analyzer Show Output", true)));
        add (hid (P::toggle (anShowRed, "Analyzer Show Reduction", true)));
        add (hid (P::toggle (anShowKey, "Analyzer Show Key", false)));
    }
    add (P::toggle (rmsOn, "RMS ON"));
    add (pct (midiShape, "MIDI Key Shape", -100.0, 100.0, 0.0));
    {
        std::vector<std::string> channels { "All" };
        for (int c = 1; c <= 16; ++c) channels.push_back (std::to_string (c));
        add (fixed (P::choice (midiChannel, "MIDI Key Channel", std::move (channels), 0)));
    }
    add (P::continuous (rmsLength, "RMS Length", Unit::Milliseconds, 4.0, 160.0, 32.0, Curve::Exponential));
    add (P::continuous (rmsSpeed, "RMS Speed", Unit::None, 0.0, 8.0, 1.0));
    add (pct (rmsMix, "RMS Mix", 0.0, 100.0, 50.0));
    for (int b = 0; b < kBands; ++b)
    {
        const std::string n = "Side Filter " + std::to_string (b + 1) + " ";
        add (P::choice (bandParam (b, kStatus), n + "Status", { "Off", "Bypass", "On" }, 0));
        add (P::choice (bandParam (b, kType), n + "Type", { "Peak", "Low Shelf", "Low Pass", "High Shelf", "High Pass", "Notch", "Band Pass", "Tilt Shelf" }, 0));
        add (P::choice (bandParam (b, kOrder), n + "Order", { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" }, 1));
        add (P::continuous (bandParam (b, kFreq), n + "Freq", Unit::Hertz, 10.0, 30000.0, 1000.0, Curve::Exponential));
        add (db (bandParam (b, kGain), (n + "Gain").c_str(), -30.0, 30.0, 0.0));
        add (P::continuous (bandParam (b, kQ), n + "Q", Unit::None, 0.025, 25.0, 0.707, Curve::Exponential));
    }
    return ok;
}

void CompressorModule::bind (const ParameterSet& set)
{
    for (std::size_t i = 0; i < idx_.size(); ++i) idx_[i] = set.indexOf (static_cast<ParamId> (100 + i));
}

int CompressorModule::latencySamples (const ParameterSet& params) const noexcept
{
    auto at = [&] (ParamId id) { return idx_[static_cast<std::size_t> (id - 100)] == ParameterSet::npos ? 0.0 : params.getAt (idx_[static_cast<std::size_t> (id - 100)]); };
    const int os = zero_.load (std::memory_order_relaxed) ? 0 : static_cast<int> (std::lround (at (oversample)));
    const int f = os >= 2 ? 4 : os == 1 ? 2 : 1;
    const double look = zero_.load (std::memory_order_relaxed) ? std::min (at (lookahead), 0.0) : at (lookahead);
    const int main = look > 0.0 ? static_cast<int> (std::lround (std::min (look, maxLookaheadMs) * 0.001 * sampleRate_)) : 0;
    return main + Oversampler::latencyFor (f);
}

int CompressorModule::maxLatencySamples (double sampleRate) const noexcept
{
    return static_cast<int> (std::ceil (maxLookaheadMs * 0.001 * sampleRate)) + Oversampler::kLatency4x + 2;
}

void CompressorModule::prepare (double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max (1, maxBlockSize);
    const auto n = static_cast<std::size_t> (maxBlock_);
    midiBuf_.assign (n, 0.0f);
    for (auto* v : { &sideL_, &sideR_, &preL_, &preR_, &postL_, &postR_, &rmsL_, &rmsR_, &sideEqL_, &sideEqR_ }) v->assign (4 * n, 0.0f);
    for (auto& o : os_) o.prepare (maxBlock_);
    const auto lookMax = static_cast<std::size_t> (std::ceil (maxLookaheadMs * 0.001 * sampleRate_)) + 2;
    for (auto& d : lookMain_) d.prepare (lookMax);
    for (auto& d : lookSide_) d.prepare (lookMax);
    for (auto& d : preDelay_) d.prepare (static_cast<std::size_t> (Oversampler::kLatency4x) + 2);
    for (auto& d : keyDelay_) d.prepare (static_cast<std::size_t> (Oversampler::kLatency4x) + 2);
    keyAnL_.assign (4 * n, 0.0f);
    keyAnR_.assign (4 * n, 0.0f);
    analyzer_.setSampleRate (sampleRate_);
    analyzer_.setMaxBlock (maxBlock_);
    const auto hiMax = static_cast<std::size_t> (std::ceil (sampleRate_ * 4.0));
    for (auto& h : hold_) h.setCapacity (hiMax);                       // 1 s à la cadence suréchantillonnée 4x
    for (auto& t : tracker_) t.allocate (static_cast<std::size_t> (0.16 * sampleRate_ * 4.0) + 8);
    reset();
}

void CompressorModule::reset() noexcept
{
    for (auto& o : os_) o.reset();
    for (auto& d : lookMain_) d.reset();
    for (auto& d : lookSide_) d.reset();
    for (auto& d : preDelay_) d.reset();
    for (auto& d : keyDelay_) d.reset();
    for (auto& h : hold_) h.clear();
    for (auto& t : tracker_) t.reset();
    for (auto& f : follower_) f.reset();
    for (auto& f : rmsFollower_) f.reset();
    for (auto& ch : sideFilters_) for (auto& f : ch) f.reset();
    feedback_ = { 0.0, 0.0 };
    haveApplied_ = false;
    bandsDirty_ = true;
    makeupGain_ = makeupTarget_ = 1.0;
    for (auto& m : meters_) m.store (0.0f, std::memory_order_relaxed);
}

void CompressorModule::readSettings (const ParameterSet& params) noexcept
{
    for (std::size_t i = 0; i < idx_.size(); ++i)
        if (idx_[i] != ParameterSet::npos) values_[i] = params.getAt (idx_[i]);
    Settings s;
    s.style = static_cast<dyn::Style> (std::clamp (static_cast<int> (std::lround (param (style))), 0, 3));
    s.direction = static_cast<dyn::Direction> (std::clamp (static_cast<int> (std::lround (param (direction))), 0, 3));
    s.threshold = param (threshold);
    s.ratio = param (ratio);
    s.knee = param (kneeWidth);
    s.curve = param (curve) * 0.02 - 1.0;
    s.floorDb = param (floorDb);
    s.attack = param (attack);
    s.release = param (release);
    s.pump = param (pump) * 0.01;
    s.smooth = param (smooth) * 0.01;
    s.range = param (range);
    s.rangeInf = param (rangeInf) >= 0.5;
    s.holdMs = param (hold);
    s.makeup = param (makeup);
    s.wet = param (wet) * 0.01;
    const int source = static_cast<int> (std::lround (param (externalSide)));
    s.ext = source == kSourceExternal;
    s.midi = source == kSourceMidi;
    s.midiShape = param (midiShape);
    s.midiChannel = std::clamp (static_cast<int> (std::lround (param (midiChannel))), 0, 16);
    s.sideOut = param (sideOut) >= 0.5;
    s.eqBypass = param (sideEqBypass) >= 0.5;
    s.sideGainDb = param (sideGain);
    const int sm = static_cast<int> (std::lround (param (stereoMode)));
    s.midSide = sm == 0 || sm == 2;
    s.stereoMax = sm == 2 || sm == 3;
    s.swap = param (stereoSwap) >= 0.5;
    s.link = 1.0 - param (stereoLink) * 0.005;
    s.wet1 = param (wet1) * 0.01;
    s.wet2 = param (wet2) * 0.01;
    s.deltaOn = param (delta) >= 0.5;
    s.clipper = param (clipperDrive);
    const bool zeroLat = zero_.load (std::memory_order_relaxed);   // profil Live : ni suréchantillonnage ni anticipation du signal (une anticipation négative, sur la clé, reste sans latence)
    s.oversample = zeroLat ? 0 : static_cast<int> (std::lround (param (oversample)));
    s.lookaheadMs = zeroLat ? std::min (param (lookahead), 0.0) : param (lookahead);
    s.rmsOn = param (rmsOn) >= 0.5;
    s.rmsLengthMs = param (rmsLength);
    s.rmsSpeed = param (rmsSpeed);
    s.rmsMix = param (rmsMix) * 0.01;
    // Expansion et gonflement : seuls Clean et Classic ont un sens (comme ZL)
    if ((s.direction == dyn::Direction::Expand || s.direction == dyn::Direction::Inflate) && s.style != dyn::Style::Clean && s.style != dyn::Style::Classic)
        s.style = dyn::Style::Clean;
    settings_ = s;
    for (int b = 0; b < kBands; ++b)
    {
        SideBand sb;
        sb.status = static_cast<int> (std::lround (param (bandParam (b, kStatus))));
        sb.type = static_cast<int> (std::lround (param (bandParam (b, kType))));
        sb.order = static_cast<int> (std::lround (param (bandParam (b, kOrder))));
        sb.freq = param (bandParam (b, kFreq));
        sb.gain = param (bandParam (b, kGain));
        sb.q = param (bandParam (b, kQ));
        if (! sb.same (bands_[static_cast<std::size_t> (b)])) { bands_[static_cast<std::size_t> (b)] = sb; bandsDirty_ = true; }
    }
}

void CompressorModule::designSideBand (int b, int c) noexcept
{
    const auto& sb = bands_[static_cast<std::size_t> (b)];
    auto& f = sideFilters_[static_cast<std::size_t> (c)][static_cast<std::size_t> (b)];
    const double fs = sampleRate_, q = sb.q;
    const int slope = 6 * (sb.order <= 1 ? sb.order + 1 : sb.order == 2 ? 4 : sb.order == 3 ? 8 : sb.order == 4 ? 12 : 16);   // 6, 12, 24, 48, 72, 96 dB/oct
    f.butterworth = false;
    f.count = 1;
    switch (static_cast<FilterType> (sb.type))
    {
        case kLowPass: f.butterworth = true; f.cascade.design (false, fs, sb.freq, slope); break;
        case kHighPass: f.butterworth = true; f.cascade.design (true, fs, sb.freq, slope); break;
        case kPeak: f.sections[0].set (BiquadCoefs::peaking (fs, sb.freq, sb.gain, q)); break;
        case kLowShelf: f.sections[0].set (BiquadCoefs::lowShelf (fs, sb.freq, sb.gain, q)); break;
        case kHighShelf: f.sections[0].set (BiquadCoefs::highShelf (fs, sb.freq, sb.gain, q)); break;
        case kNotch: f.sections[0].set (BiquadCoefs::notch (fs, sb.freq, q)); break;
        case kBandPass: f.sections[0].set (BiquadCoefs::bandpass (fs, sb.freq, q)); break;
        case kTiltShelf:
            f.count = 2;   // inclinaison : étagère grave +g/2 et étagère aiguë -g/2 à la même fréquence
            f.sections[0].set (BiquadCoefs::lowShelf (fs, sb.freq, sb.gain * 0.5, 0.707));
            f.sections[1].set (BiquadCoefs::highShelf (fs, sb.freq, -sb.gain * 0.5, 0.707));
            break;
    }
}

void CompressorModule::applySettings() noexcept
{
    const auto& s = settings_;
    const int factor = s.oversample >= 2 ? 4 : s.oversample == 1 ? 2 : 1;
    const double hiRate = sampleRate_ * factor;
    const bool rateChanged = ! haveApplied_ || hiRate != appliedRate_ || factor != factor_;
    if (factor != factor_)
    {
        factor_ = factor;
        for (auto& o : os_) o.setFactor (factor);
    }
    if (rateChanged)
    {
        appliedRate_ = hiRate;
        for (auto& f : follower_) f.prepare (hiRate);
        for (auto& f : rmsFollower_) f.prepare (hiRate);
        for (auto& t : tracker_) t.setRate (hiRate);
        for (auto& h : hold_) h.clear();
        const auto hold = static_cast<std::size_t> (hiRate * s.holdMs * 0.001);
        for (auto& h : hold_) h.setSize (hold);
    }
    if (! haveApplied_ || s.style != appliedStyle_ || s.direction != appliedDirection_)
    {
        for (auto& f : follower_) f.reset();
        for (auto& f : rmsFollower_) f.reset();
        feedback_ = { 0.0, 0.0 };
        if (haveApplied_ && s.direction != appliedDirection_) for (auto& h : hold_) h.clear();
        appliedStyle_ = s.style;
        appliedDirection_ = s.direction;
    }
    haveApplied_ = true;
    for (auto& h : hold_) h.setSize (static_cast<std::size_t> (hiRate * s.holdMs * 0.001));
    computer_.set (s.direction, s.threshold, s.ratio, s.knee, s.curve, s.floorDb);
    for (auto& f : follower_) f.set (s.attack, s.release, s.pump, s.smooth);
    for (auto& f : rmsFollower_) f.set (s.attack * s.rmsSpeed, s.release * s.rmsSpeed, 0.0, 0.0);
    for (auto& t : tracker_) t.setSeconds (s.rmsLengthMs * 0.001);
    const double unit = s.direction == dyn::Direction::Compress ? computer_.eval (0.0) : -2.360900573208;
    clipper_.set (s.clipper, unit);
    makeupTarget_ = dyn::decibelsToGain (s.makeup * s.wet);
    for (auto& d : lookMain_) d.setDelay (s.lookaheadMs > 0.0 ? static_cast<std::size_t> (std::lround (std::min (s.lookaheadMs, maxLookaheadMs) * 0.001 * sampleRate_)) : 0);
    for (auto& d : lookSide_) d.setDelay (s.lookaheadMs < 0.0 ? static_cast<std::size_t> (std::lround (std::min (-s.lookaheadMs, maxLookaheadMs) * 0.001 * sampleRate_)) : 0);
    for (auto& d : preDelay_) d.setDelay (static_cast<std::size_t> (Oversampler::latencyFor (factor_)));
    for (auto& d : keyDelay_) d.setDelay (static_cast<std::size_t> (Oversampler::latencyFor (factor_)));
    if (bandsDirty_)
    {
        for (int b = 0; b < kBands; ++b)
            for (int c = 0; c < 2; ++c) designSideBand (b, c);
        bandsDirty_ = false;
    }
}

double CompressorModule::curveOutputDb (const ParameterSet& params, double inputDb) const noexcept
{
    auto at = [&] (ParamId id) { return idx_[static_cast<std::size_t> (id - 100)] == ParameterSet::npos ? 0.0 : params.getAt (idx_[static_cast<std::size_t> (id - 100)]); };
    dyn::GainComputer c;
    c.set (static_cast<dyn::Direction> (std::clamp (static_cast<int> (std::lround (at (direction))), 0, 3)), at (threshold), at (ratio), at (kneeWidth),
           at (curve) * 0.02 - 1.0, at (floorDb));
    return c.evalOutput (inputDb);
}

double CompressorModule::sideBandResponseDb (const ParameterSet& params, int b, double f, double fs) noexcept
{
    if (b < 0 || b >= kBands || std::lround (params.get (bandParam (b, kStatus))) != kOn) return 0.0;
    const int type = static_cast<int> (std::lround (params.get (bandParam (b, kType))));
    const int order = static_cast<int> (std::lround (params.get (bandParam (b, kOrder))));
    const double freq = params.get (bandParam (b, kFreq)), gain = params.get (bandParam (b, kGain)), q = params.get (bandParam (b, kQ));
    const int slope = 6 * (order <= 1 ? order + 1 : order == 2 ? 4 : order == 3 ? 8 : order == 4 ? 12 : 16);
    double mag = 1.0;
    switch (static_cast<FilterType> (type))
    {
        case kLowPass: case kHighPass:
        {
            ButterworthCascade c;
            c.design (type == kHighPass, fs, freq, slope);
            mag = c.magnitude (fs, f);
            break;
        }
        case kPeak: mag = BiquadCoefs::peaking (fs, freq, gain, q).magnitude (fs, f); break;
        case kLowShelf: mag = BiquadCoefs::lowShelf (fs, freq, gain, q).magnitude (fs, f); break;
        case kHighShelf: mag = BiquadCoefs::highShelf (fs, freq, gain, q).magnitude (fs, f); break;
        case kNotch: mag = BiquadCoefs::notch (fs, freq, q).magnitude (fs, f); break;
        case kBandPass: mag = BiquadCoefs::bandpass (fs, freq, q).magnitude (fs, f); break;
        case kTiltShelf:
            mag = BiquadCoefs::lowShelf (fs, freq, gain * 0.5, 0.707).magnitude (fs, f) * BiquadCoefs::highShelf (fs, freq, -gain * 0.5, 0.707).magnitude (fs, f);
            break;
    }
    return 20.0 * std::log10 (std::max (mag, 1.0e-9));
}

double CompressorModule::sideResponseDb (const ParameterSet& params, double f, double fs) noexcept
{
    double sum = 0.0;
    for (int b = 0; b < kBands; ++b) sum += sideBandResponseDb (params, b, f, fs);
    return sum;
}

// Réduction (dB) pour un tampon de clé, selon le style ; remplace le contenu du tampon.
void CompressorModule::styleProcess (int c, float* buf, int n) noexcept
{
    auto& fol = follower_[static_cast<std::size_t> (c)];
    auto& x0 = feedback_[static_cast<std::size_t> (c)];
    switch (settings_.style)
    {
        case dyn::Style::Clean:
            for (int i = 0; i < n; ++i)
            {
                const double x = std::max (std::fabs (static_cast<double> (buf[i])), dyn::kLogMin);
                buf[i] = static_cast<float> (-fol.process (-computer_.eval (std::log (x) * dyn::kLogMul)));
            }
            break;
        case dyn::Style::Classic:
            for (int i = 0; i < n; ++i)
            {
                const double r = -fol.process (-computer_.eval (dyn::gainToDecibels (std::fabs (x0))));
                x0 = static_cast<double> (buf[i]) * dyn::decibelsToGain (r);
                buf[i] = static_cast<float> (r);
            }
            break;
        case dyn::Style::Optical:
            for (int i = 0; i < n; ++i)
            {
                const double v = fol.process (std::fabs (static_cast<double> (buf[i])));
                buf[i] = static_cast<float> (computer_.eval (dyn::gainToDecibels (v)));
            }
            break;
        case dyn::Style::Vocal:
            for (int i = 0; i < n; ++i)
            {
                const double g = -fol.process (-dyn::decibelsToGain (computer_.eval (dyn::gainToDecibels (std::fabs (x0)))));
                x0 = static_cast<double> (buf[i]) * g;
                buf[i] = static_cast<float> (dyn::gainToDecibels (std::fabs (g)));
            }
            break;
    }
}

// Branche RMS (style Clean sur l'énergie moyenne de la clé).
void CompressorModule::rmsProcess (int c, const float* in, float* out, int n) noexcept
{
    auto& tr = tracker_[static_cast<std::size_t> (c)];
    auto& fol = rmsFollower_[static_cast<std::size_t> (c)];
    for (int i = 0; i < n; ++i)
    {
        tr.process (static_cast<double> (in[i]));
        const double ms = std::max (tr.meanSquare(), dyn::kLogSqrMin);
        out[i] = static_cast<float> (-fol.process (-computer_.eval (std::log (ms) * dyn::kLogSqrMul)));
    }
}

void CompressorModule::processHighRate (float* mainL, float* mainR, float* sideL, float* sideR, int n) noexcept
{
    const auto& s = settings_;
    // réduction par canal (dans le tampon de clé)
    styleProcess (0, sideL, n);
    styleProcess (1, sideR, n);
    if (s.rmsOn)
    {
        // la branche RMS part de la clé d'origine : on la recopie avant le style (déjà remplacée) via les tampons dédiés
        rmsProcess (0, rmsL_.data(), rmsL_.data(), n);
        rmsProcess (1, rmsR_.data(), rmsR_.data(), n);
        for (int i = 0; i < n; ++i)
        {
            sideL[i] = static_cast<float> (s.rmsMix * (rmsL_[static_cast<std::size_t> (i)] - sideL[i]) + sideL[i]);
            sideR[i] = static_cast<float> (s.rmsMix * (rmsR_[static_cast<std::size_t> (i)] - sideR[i]) + sideR[i]);
        }
    }
    // maintien : réduction la plus forte sur la fenêtre
    if (hold_[0].size() > 0)
        for (int i = 0; i < n; ++i)
        {
            sideL[i] = hold_[0].push (sideL[i]);
            sideR[i] = hold_[1].push (sideR[i]);
        }
    // liaison stéréo
    const double link = s.link, linkMax = 1.0 - 2.0 * (1.0 - link);
    if (s.stereoMax)
        for (int i = 0; i < n; ++i)
        {
            const float x = sideL[i], y = sideR[i];
            if (x < y) sideR[i] = static_cast<float> ((y - x) * linkMax + x);
            else sideL[i] = static_cast<float> ((x - y) * linkMax + y);
        }
    else
        for (int i = 0; i < n; ++i)
        {
            const float x = sideL[i], y = sideR[i];
            const float xy = static_cast<float> (link * (x - y));
            sideL[i] = y + xy;
            sideR[i] = x - xy;
        }
    // gain : plage, wet, conversion dB -> linéaire, échange éventuel
    const bool downward = s.direction == dyn::Direction::Compress || s.direction == dyn::Direction::Expand;
    const double w1 = (downward ? 1.0 : -1.0) * s.wet1 * s.wet * 0.05 * kLn10, w2 = (downward ? 1.0 : -1.0) * s.wet2 * s.wet * 0.05 * kLn10;
    const double rangeDb = s.range * s.wet;
    const float lo = s.rangeInf ? -240.0f : static_cast<float> (-rangeDb), hi = s.rangeInf ? 40.0f : static_cast<float> (std::min (40.0, rangeDb));
    float minRed0 = 0.0f, minRed1 = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = std::clamp (sideL[i], lo, hi), b = std::clamp (sideR[i], lo, hi);
        minRed0 = std::min (minRed0, a);
        minRed1 = std::min (minRed1, b);
        const float g0 = static_cast<float> (std::exp (static_cast<double> (a) * w1)), g1 = static_cast<float> (std::exp (static_cast<double> (b) * w2));
        if (s.swap) { mainL[i] *= g1; mainR[i] *= g0; }
        else { mainL[i] *= g0; mainR[i] *= g1; }
    }
    meters_[kReductionL].store (minRed0, std::memory_order_relaxed);
    meters_[kReductionR].store (minRed1, std::memory_order_relaxed);
    if (clipper_.on())
        for (int i = 0; i < n; ++i)
        {
            mainL[i] = static_cast<float> (clipper_.process (mainL[i]));
            mainR[i] = static_cast<float> (clipper_.process (mainR[i]));
        }
}

void CompressorModule::process (float* left, float* right, int n, const ParameterSet& params) noexcept
{
    if (n <= 0 || n > maxBlock_) return;
    readSettings (params);
    applySettings();
    const auto& s = settings_;
    const auto un = static_cast<std::size_t> (n);
    float* const main[2] = { left, right };
    float* const side[2] = { sideL_.data(), sideR_.data() };
    float peakIn[2] = { 0.0f, 0.0f };
    for (int i = 0; i < n; ++i) { peakIn[0] = std::max (peakIn[0], std::fabs (left[i])); peakIn[1] = std::max (peakIn[1], std::fabs (right[i])); }

    // clé : externe si demandée et branchée, sinon le signal lui-même ; gain puis filtres de détection. Source MIDI : clé synthétique constante,
    // sans gain ni filtres (le niveau vient de la vélocité).
    const bool useExt = s.ext && keyL_ != nullptr && keyR_ != nullptr;
    if (s.midi)
    {
        midiKey_.render (midiBuf_.data(), n, s.midiChannel, s.midiShape);
        std::copy (midiBuf_.begin(), midiBuf_.begin() + n, side[0]);
        std::copy (midiBuf_.begin(), midiBuf_.begin() + n, side[1]);
    }
    else
    {
        const float sideGain = static_cast<float> (dyn::decibelsToGain (s.sideGainDb));
        for (int i = 0; i < n; ++i)
        {
            side[0][i] = (useExt ? keyL_[i] : left[i]) * sideGain;
            side[1][i] = (useExt ? keyR_[i] : right[i]) * sideGain;
        }
    }
    if (! s.eqBypass && ! s.midi)
        for (int b = 0; b < kBands; ++b)
            if (bands_[static_cast<std::size_t> (b)].status == kOn)
                for (int c = 0; c < 2; ++c)
                {
                    auto& f = sideFilters_[static_cast<std::size_t> (c)][static_cast<std::size_t> (b)];
                    for (int i = 0; i < n; ++i) side[c][i] = f.process (side[c][i]);
                }
    std::copy (side[0], side[0] + n, sideEqL_.begin());
    std::copy (side[1], side[1] + n, sideEqR_.begin());

    // lookahead : retarde le signal (latence) ou la clé (sans latence)
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            main[c][i] = lookMain_[static_cast<std::size_t> (c)].process (main[c][i]);
            side[c][i] = lookSide_[static_cast<std::size_t> (c)].process (side[c][i]);
        }
    // signal d'entrée aligné sur la latence totale (pour DELTA)
    for (int i = 0; i < n; ++i)
    {
        preL_[static_cast<std::size_t> (i)] = preDelay_[0].process (main[0][i]);
        preR_[static_cast<std::size_t> (i)] = preDelay_[1].process (main[1][i]);
        keyAnL_[static_cast<std::size_t> (i)] = keyDelay_[0].process (side[0][i]);
        keyAnR_[static_cast<std::size_t> (i)] = keyDelay_[1].process (side[1][i]);
    }
    // M/S
    if (s.midSide)
        for (int i = 0; i < n; ++i)
        {
            const float ml = 0.5f * (main[0][i] + main[1][i]);
            main[1][i] = ml - main[1][i];
            main[0][i] = ml;
            const float sl = 0.5f * (side[0][i] + side[1][i]);
            side[1][i] = sl - side[1][i];
            side[0][i] = sl;
        }
    // branche RMS : copie de la clé d'origine (avant que le style ne la remplace), à la cadence suréchantillonnée
    float* hMainL = os_[0].up (main[0], n);
    float* hMainR = os_[1].up (main[1], n);
    float* hSideL = os_[2].up (side[0], n);
    float* hSideR = os_[3].up (side[1], n);
    const int hn = n * factor_;
    if (s.rmsOn)
    {
        std::copy (hSideL, hSideL + hn, rmsL_.begin());
        std::copy (hSideR, hSideR + hn, rmsR_.begin());
    }
    processHighRate (hMainL, hMainR, hSideL, hSideR, hn);
    os_[0].down (main[0], n);
    os_[1].down (main[1], n);
    if (s.midSide)
        for (int i = 0; i < n; ++i)
        {
            const float m = main[0][i], sd = main[1][i];
            main[0][i] = m + sd;
            main[1][i] = m - sd;
        }
    std::copy (main[0], main[0] + n, postL_.begin());
    std::copy (main[1], main[1] + n, postR_.begin());
    // gain de compensation, en rampe
    {
        const double step = (makeupTarget_ - makeupGain_) / static_cast<double> (n);
        for (int i = 0; i < n; ++i, makeupGain_ += step)
        {
            main[0][i] = static_cast<float> (main[0][i] * makeupGain_);
            main[1][i] = static_cast<float> (main[1][i] * makeupGain_);
        }
        makeupGain_ = makeupTarget_;
    }
    // analyseur : entrée alignée, sortie compressée, sortie finale (avant delta et écoute de la clé), clé alignée
    if (analyzer_.active())
    {
        const float* const streams[8] = { preL_.data(), preR_.data(), postL_.data(), postR_.data(), main[0], main[1], keyAnL_.data(), keyAnR_.data() };
        analyzer_.push (streams, n);
    }
    if (s.deltaOn)
        for (int i = 0; i < n; ++i)
        {
            main[0][i] = preL_[static_cast<std::size_t> (i)] - postL_[static_cast<std::size_t> (i)];
            main[1][i] = preR_[static_cast<std::size_t> (i)] - postR_[static_cast<std::size_t> (i)];
        }
    if (s.sideOut && ! s.midi)   // écouter une clé MIDI constante enverrait du continu à la sortie
        for (int i = 0; i < n; ++i) { main[0][i] = sideEqL_[static_cast<std::size_t> (i)]; main[1][i] = sideEqR_[static_cast<std::size_t> (i)]; }
    float peakOut[2] = { 0.0f, 0.0f };
    for (int i = 0; i < n; ++i) { peakOut[0] = std::max (peakOut[0], std::fabs (left[i])); peakOut[1] = std::max (peakOut[1], std::fabs (right[i])); }
    meters_[kInPeakL].store (peakIn[0], std::memory_order_relaxed);
    meters_[kInPeakR].store (peakIn[1], std::memory_order_relaxed);
    meters_[kOutPeakL].store (peakOut[0], std::memory_order_relaxed);
    meters_[kOutPeakR].store (peakOut[1], std::memory_order_relaxed);
    (void) un;
}
} // namespace neptune::core
