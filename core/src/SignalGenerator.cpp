#include "neptune/core/SignalGenerator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace neptune::core::generator
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr const char* kSignalNames[kNumSignals] = { "Sine", "Square", "Triangle", "Saw up", "Saw down", "Impulse", "Two-tone", "DC", "White noise", "Pink noise", "Brown noise",
                                                    "Blue noise", "Violet noise", "Log sweep", "Linear sweep" };
constexpr const char* kDrumNames[kNumDrums] = { "Kick", "Snare", "Closed hat", "Open hat", "Tom", "Clap", "Rim", "Cowbell" };

double dbToLin (double db) noexcept { return db <= -119.999 ? 0.0 : std::pow (10.0, db / 20.0); }

// Correction PolyBLEP d'une discontinuité (t : phase du saut dans [0,1), dt : incrément de phase).
double polyBlep (double t, double dt) noexcept
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

// Bruits colorés, bruts (avant normalisation de la valeur efficace). `type` : 0 blanc, 1 rose, 2 brun, 3 bleu, 4 violet.
float colouredNoise (int type, float w, ColourState& c) noexcept
{
    switch (type)
    {
        case 0: return w;
        case 1:   // rose : filtre de Kellet (environ 0,5 dB de la pente de -3 dB par octave)
        case 3:
        {
            c.b0 = 0.99886f * c.b0 + w * 0.0555179f;
            c.b1 = 0.99332f * c.b1 + w * 0.0750759f;
            c.b2 = 0.96900f * c.b2 + w * 0.1538520f;
            c.b3 = 0.86650f * c.b3 + w * 0.3104856f;
            c.b4 = 0.55000f * c.b4 + w * 0.5329522f;
            c.b5 = -0.7616f * c.b5 - w * 0.0168980f;
            const float pink = c.b0 + c.b1 + c.b2 + c.b3 + c.b4 + c.b5 + c.b6 + w * 0.5362f;
            c.b6 = w * 0.115926f;
            if (type == 1) return pink;
            const float blue = pink - c.lastPink;   // +3 dB par octave
            c.lastPink = pink;
            return blue;
        }
        case 2: c.brown = (c.brown + 0.02f * w) / 1.02f; return c.brown;   // -6 dB par octave au-dessus de quelques hertz
        default: { const float v = w - c.lastWhite; c.lastWhite = w; return v; }   // violet : +6 dB par octave
    }
}

struct SimpleRng
{
    std::uint32_t s;
    explicit SimpleRng (std::uint32_t seed) : s (seed == 0 ? 1u : seed) {}
    float next() noexcept
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<float> (static_cast<std::int32_t> (s)) * (1.0f / 2147483648.0f);
    }
};

// Facteurs ramenant la valeur efficace de chaque bruit à 1 (mesurés sur 2 s de bruit, une fois).
const std::array<float, 5>& noiseNorm()
{
    static const std::array<float, 5> norm = []
    {
        std::array<float, 5> out {};
        for (int type = 0; type < 5; ++type)
        {
            SimpleRng rng (12345u);
            ColourState c;
            for (int i = 0; i < 8000; ++i) colouredNoise (type, rng.next(), c);   // mise en régime
            double sum = 0.0;
            constexpr int kN = 96000 * 2;
            for (int i = 0; i < kN; ++i) { const double v = colouredNoise (type, rng.next(), c); sum += v * v; }
            const double rms = std::sqrt (sum / kN);
            out[static_cast<std::size_t> (type)] = rms > 1.0e-9 ? static_cast<float> (1.0 / rms) : 1.0f;
        }
        return out;
    }();
    return norm;
}

constexpr double kFadeSeconds = 0.004;
} // namespace

const char* signalName (int s) noexcept { return s >= 0 && s < kNumSignals ? kSignalNames[s] : ""; }
const char* drumName (int d) noexcept { return d >= 0 && d < kNumDrums ? kDrumNames[d] : ""; }

bool addParams (ParameterSet& set, std::string* error)
{
    using P = ParamSpec;
    bool ok = true;
    auto add = [&] (P s) { ok = ok && set.add (std::move (s), error); };
    auto db = [] (ParamId id, const char* name, double lo, double hi, double def)
    {
        auto s = P::continuous (id, name, Unit::Decibel, lo, hi, def);
        s.minText = "-inf";
        return s;
    };
    auto pct = [] (ParamId id, const char* name, double lo, double hi, double def) { return P::continuous (id, name, Unit::Percent, lo, hi, def); };

    std::vector<std::string> signals;
    for (const char* n : kSignalNames) signals.emplace_back (n);
    add (P::choice (signal, "Signal", signals, kSine));
    add (P::choice (mode, "Mode", { "Continuous", "Triggered", "MIDI note" }, kContinuous));
    add (P::toggle (on, "Signal On", false));
    add (P::continuous (freq, "Frequency", Unit::Hertz, 20.0, 20000.0, 1000.0, Curve::Exponential));
    add (db (level, "Level", -120.0, 0.0, -12.0));
    add (pct (pan, "Pan", -100.0, 100.0, 0.0));
    add (pct (width, "Pulse Width", 1.0, 99.0, 50.0));
    add (P::continuous (freq2, "Second Frequency", Unit::Hertz, 20.0, 20000.0, 7000.0, Curve::Exponential));
    add (db (level2, "Second Level", -60.0, 0.0, -12.0));
    add (P::continuous (sweepStart, "Sweep Start", Unit::Hertz, 10.0, 20000.0, 20.0, Curve::Exponential));
    add (P::continuous (sweepEnd, "Sweep End", Unit::Hertz, 10.0, 24000.0, 20000.0, Curve::Exponential));
    add (P::continuous (sweepTime, "Sweep Time", Unit::Seconds, 0.1, 60.0, 5.0, Curve::Exponential));
    add (P::continuous (length, "Length", Unit::Milliseconds, 10.0, 10000.0, 500.0, Curve::Exponential));
    add (P::continuous (burstOn, "Burst On", Unit::Milliseconds, 0.0, 2000.0, 0.0, Curve::Power, 2.0));
    add (P::continuous (burstOff, "Burst Off", Unit::Milliseconds, 0.0, 2000.0, 0.0, Curve::Power, 2.0));
    add (P::choice (polarity, "Polarity", { "Normal", "Invert left", "Invert right", "Invert both" }, kPolNormal));
    add (P::toggle (trigger, "Trigger", false));
    add (P::stepped (drumNote, "First Drum Note", Unit::None, 0, 120, 36));
    add (db (master, "Master", -96.0, 6.0, 0.0));

    add (P::toggle (seqRun, "Sequencer Run", false));
    add (P::choice (seqSync, "Sequencer Sync", { "Internal", "Host" }, kSyncHost));
    {
        auto t = P::continuous (tempo, "Tempo", Unit::None, 40.0, 300.0, 120.0);
        t.decimals = 1;
        add (std::move (t));
    }
    add (P::stepped (steps, "Steps", Unit::None, 4, kMaxSteps, kMaxSteps));
    add (pct (swing, "Swing", 0.0, 75.0, 0.0));
    add (db (drumLevel, "Drums Level", -96.0, 6.0, 0.0));

    // métronome
    add (P::toggle (metroOn, "Metronome On", false));
    add (P::choice (metroSync, "Metronome Sync", { "Internal", "Host" }, kSyncInternal));
    {
        auto t = P::continuous (metroTempo, "Metronome Tempo", Unit::None, 20.0, 300.0, 120.0);
        t.decimals = 1;
        add (std::move (t));
    }
    add (P::stepped (metroBeats, "Metronome Beats", Unit::None, 1, 12, 4));
    add (P::choice (metroUnit, "Metronome Beat Unit", { "1/4", "1/8", "1/16" }, 0));
    add (P::choice (metroSub, "Metronome Subdivision", { "1", "2", "3", "4" }, 0));
    add (P::choice (metroSound, "Metronome Sound", { "Click", "Wood", "Beep", "Bell" }, 0));
    add (db (metroLevel, "Metronome Level", -60.0, 0.0, -12.0));
    add (P::continuous (metroAccent, "Metronome Accent", Unit::Decibel, 0.0, 12.0, 6.0));
    add (P::continuous (metroSubLevel, "Metronome Subdivision Level", Unit::Decibel, -24.0, 0.0, -6.0));

    // accordeur
    {
        auto a = P::continuous (tunerA4, "Tuner Reference A4", Unit::Hertz, 415.0, 466.0, 440.0);
        a.decimals = 1;
        add (std::move (a));
    }
    {
        std::vector<std::string> names;
        for (const auto& t : tuner::tunings()) names.emplace_back (t.en);
        add (P::choice (tunerPreset, "Tuner Tuning", names, 0));
    }
    add (P::toggle (tunerTone, "Tuner Tone On", false));
    add (P::stepped (tunerNote, "Tuner Tone Note", Unit::None, 0, 127, 69));
    {
        auto c = P::continuous (tunerCents, "Tuner Tone Cents", Unit::Cents, -50.0, 50.0, 0.0);
        c.decimals = 1;
        add (std::move (c));
    }
    add (db (tunerLevel, "Tuner Tone Level", -60.0, 0.0, -18.0));

    for (int d = 0; d < kNumDrums; ++d)
    {
        const std::string n = kDrumNames[d];
        add (db (drumParam (d, kDrumLevel), (n + " Level").c_str(), -96.0, 6.0, 0.0));
        add (P::continuous (drumParam (d, kDrumTune), (n + " Tune").c_str(), Unit::Semitones, -12.0, 12.0, 0.0));
        add (P::continuous (drumParam (d, kDrumDecay), (n + " Decay").c_str(), Unit::Percent, 25.0, 400.0, 100.0, Curve::Exponential));
        add (pct (drumParam (d, kDrumPan), (n + " Pan").c_str(), -100.0, 100.0, 0.0));
        add (P::toggle (drumParam (d, kDrumMute), (n + " Mute").c_str(), false));
        for (int s = 0; s < kMaxSteps; ++s) add (P::stepped (stepParam (d, s), (n + " Step " + std::to_string (s + 1)).c_str(), Unit::None, 0, 2, 0));
    }
    return ok;
}

double GeneratorEngine::noteFrequency (int key) noexcept { return 440.0 * std::pow (2.0, (key - 69) / 12.0); }

GeneratorEngine::GeneratorEngine()
{
    std::string error;
    ok_ = addParams (params_, &error);
    if (ok_) bindIndices();
}

void GeneratorEngine::bindIndices()
{
    auto ix = [this] (ParamId id) { return params_.indexOf (id); };
    iSignal_ = ix (signal); iMode_ = ix (mode); iOn_ = ix (on); iFreq_ = ix (freq); iLevel_ = ix (level); iPan_ = ix (pan); iWidth_ = ix (width);
    iFreq2_ = ix (freq2); iLevel2_ = ix (level2); iSweepStart_ = ix (sweepStart); iSweepEnd_ = ix (sweepEnd); iSweepTime_ = ix (sweepTime);
    iLength_ = ix (length); iBurstOn_ = ix (burstOn); iBurstOff_ = ix (burstOff); iPolarity_ = ix (polarity); iTrigger_ = ix (trigger);
    iDrumNote_ = ix (drumNote); iMaster_ = ix (master);
    iMetroOn_ = ix (metroOn); iMetroSync_ = ix (metroSync); iMetroTempo_ = ix (metroTempo); iMetroBeats_ = ix (metroBeats); iMetroUnit_ = ix (metroUnit);
    iMetroSub_ = ix (metroSub); iMetroSound_ = ix (metroSound); iMetroLevel_ = ix (metroLevel); iMetroAccent_ = ix (metroAccent); iMetroSubLevel_ = ix (metroSubLevel);
    iTunerA4_ = ix (tunerA4); iTunerTone_ = ix (tunerTone); iTunerNote_ = ix (tunerNote); iTunerCents_ = ix (tunerCents); iTunerLevel_ = ix (tunerLevel);
    iSeqRun_ = ix (seqRun); iSeqSync_ = ix (seqSync); iTempo_ = ix (tempo); iSteps_ = ix (steps); iSwing_ = ix (swing); iDrumLevel_ = ix (drumLevel);
    for (int d = 0; d < kNumDrums; ++d)
    {
        for (int k = 0; k < 5; ++k) iDrum_[static_cast<std::size_t> (d)][static_cast<std::size_t> (k)] = ix (drumParam (d, k));
        for (int s = 0; s < kMaxSteps; ++s) iStep_[static_cast<std::size_t> (d)][static_cast<std::size_t> (s)] = ix (stepParam (d, s));
    }
}

void GeneratorEngine::prepare (double sampleRate, int maxBlockSize)
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max (1, maxBlockSize);
    (void) noiseNorm();   // calibration des bruits, hors temps réel
    tracker_.prepare (sr_);
    tone_.prepare (sr_);
    metro_.prepare (sr_);
    aux_.assign (static_cast<std::size_t> (std::max (maxBlock_, 8192)), 0.0f);
    reset();
}

void GeneratorEngine::reset() noexcept
{
    colour_ = ColourState {};
    rng_ = Rng {};
    phase_ = phase2_ = 0.0;
    tSig_ = tBurst_ = 0.0;
    remain_ = -1;
    gate_ = false;
    lastOn_ = false;
    lastTrigger_ = false;
    lastMode_ = -1;
    gain_ = 0.0f;
    smInit_ = false;
    heldCount_ = 0;
    noteMode_ = false;
    for (auto& v : voices_) v = DrumVoice {};
    seqPlaying_ = false;
    seqPpq_ = 0.0;
    lastStepGlobal_ = std::numeric_limits<long long>::min();
    step_.store (-1, std::memory_order_relaxed);
    lastTriggerCount_ = triggerCount_.load (std::memory_order_relaxed);
    lastPanicCount_ = panicCount_.load (std::memory_order_relaxed);
    for (std::size_t d = 0; d < static_cast<std::size_t> (kNumDrums); ++d) lastDrumCount_[d] = drumCount_[d].load (std::memory_order_relaxed);
    sounding_.store (false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------- signal
void GeneratorEngine::startSignal (double freqHz, bool fromNote) noexcept
{
    const int sig = static_cast<int> (std::lround (p (iSignal_)));
    const int md = static_cast<int> (std::lround (p (iMode_)));
    tSig_ = 0.0;
    tBurst_ = 0.0;
    phase_ = sig == kImpulse ? 1.0 : 0.0;
    phase2_ = 0.0;
    noteMode_ = fromNote;
    noteFreq_ = freqHz;
    gate_ = true;
    if (md == kTriggered)
    {
        const bool sweep = sig == kSweepLog || sig == kSweepLin;
        const double seconds = sweep ? p (iSweepTime_) : p (iLength_) * 0.001;
        remain_ = static_cast<long long> (seconds * sr_);
    }
    else remain_ = -1;
}

void GeneratorEngine::noteOn (int key, float velocity) noexcept
{
    const int base = static_cast<int> (std::lround (p (iDrumNote_)));
    if (key >= base && key < base + kNumDrums)
    {
        triggerDrum (key - base, std::clamp (velocity, 0.0f, 1.0f));
        return;
    }
    const int md = static_cast<int> (std::lround (p (iMode_)));
    if (md == kTriggered) startSignal (p (iFreq_), false);
    else if (md == kMidi)
    {
        if (heldCount_ == static_cast<int> (held_.size()))
        {
            for (std::size_t i = 1; i < held_.size(); ++i) held_[i - 1] = held_[i];
            --heldCount_;
        }
        held_[static_cast<std::size_t> (heldCount_++)] = key;
        const double f = noteFrequency (key);
        if (gate_ && remain_ < 0 && noteMode_) noteFreq_ = f;   // legato : la fréquence change sans repartir
        else startSignal (f, true);
    }
}

void GeneratorEngine::noteOff (int key) noexcept
{
    int w = 0;
    for (int i = 0; i < heldCount_; ++i)
        if (held_[static_cast<std::size_t> (i)] != key) held_[static_cast<std::size_t> (w++)] = held_[static_cast<std::size_t> (i)];
    const bool had = w != heldCount_;
    heldCount_ = w;
    if (! had || static_cast<int> (std::lround (p (iMode_))) != kMidi) return;
    if (heldCount_ == 0) gate_ = false;
    else noteFreq_ = noteFrequency (held_[static_cast<std::size_t> (heldCount_ - 1)]);
}

void GeneratorEngine::allNotesOff() noexcept
{
    heldCount_ = 0;
    if (static_cast<int> (std::lround (p (iMode_))) == kMidi) gate_ = false;
}

// ---------------------------------------------------------------------------------------------------------------- batterie
void GeneratorEngine::triggerDrum (int drum, float velocity) noexcept
{
    if (drum < 0 || drum >= kNumDrums) return;
    auto& v = voices_[static_cast<std::size_t> (drum)];
    const std::size_t d = static_cast<std::size_t> (drum);
    v = DrumVoice {};
    v.active = true;
    v.velocity = velocity;
    v.tune = static_cast<float> (std::pow (2.0, p (iDrum_[d][kDrumTune]) / 12.0));
    v.decay = static_cast<float> (p (iDrum_[d][kDrumDecay]) * 0.01);
    v.seed = 0x9E3779B9u * static_cast<std::uint32_t> (drum + 1) + static_cast<std::uint32_t> (rng_.s);
    switch (drum)
    {
        case kSnare: v.f1.set (BiquadCoefs::highpass (sr_, 1800.0, 0.7)); break;
        case kHatClosed:
        case kHatOpen: v.f1.set (BiquadCoefs::highpass (sr_, 7000.0, 0.7)); v.f2.set (BiquadCoefs::highpass (sr_, 7000.0, 0.7)); break;
        case kClap: v.f1.set (BiquadCoefs::bandpass (sr_, 1200.0, 1.0)); break;
        case kCowbell: v.f1.set (BiquadCoefs::bandpass (sr_, 800.0 * v.tune, 1.5)); break;
        default: break;
    }
    if (drum == kHatClosed) voices_[static_cast<std::size_t> (kHatOpen)].active = false;   // le charleston fermé coupe l'ouvert
}

float GeneratorEngine::renderDrum (int drum, DrumVoice& v) noexcept
{
    const double t = v.t;
    const double ts = t / sr_;   // secondes
    v.t += 1.0;
    auto noise = [&v]() noexcept
    {
        v.seed ^= v.seed << 13; v.seed ^= v.seed >> 17; v.seed ^= v.seed << 5;
        return static_cast<float> (static_cast<std::int32_t> (v.seed)) * (1.0f / 2147483648.0f);
    };
    const double dec = v.decay, tune = v.tune;
    double y = 0.0, tail = 0.0;   // tail : durée après laquelle la voix est finie
    switch (drum)
    {
        case kKick:
        {
            const double f = 48.0 * tune + (170.0 - 48.0) * tune * std::exp (-ts / 0.03);
            v.ph[0] += f / sr_;
            const double env = std::exp (-ts / (0.28 * dec));
            y = std::sin (kTwoPi * v.ph[0]) * env + 0.35 * noise() * std::exp (-ts / 0.0008);
            tail = 0.28 * dec;
            break;
        }
        case kSnare:
        {
            v.ph[0] += 180.0 * tune / sr_;
            const double tone = std::sin (kTwoPi * v.ph[0]) * std::exp (-ts / (0.07 * dec)) * 0.5;
            const double nz = v.f1.process (noise()) * std::exp (-ts / (0.11 * dec)) * 0.7;
            y = tone + nz;
            tail = 0.11 * dec;
            break;
        }
        case kHatClosed:
        case kHatOpen:
        {
            static constexpr double kFreq[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
            double s = 0.0;
            for (int i = 0; i < 6; ++i)
            {
                v.ph[i] += kFreq[i] * 2.0 * tune / sr_;
                v.ph[i] -= std::floor (v.ph[i]);
                s += v.ph[i] < 0.5 ? 1.0 : -1.0;
            }
            s = s / 6.0 * 0.7 + noise() * 0.3;
            const double tau = (drum == kHatClosed ? 0.025 : 0.18) * dec;
            y = v.f2.process (v.f1.process (static_cast<float> (s))) * std::exp (-ts / tau) * 0.8;
            tail = tau;
            break;
        }
        case kClap:
        {
            double env;
            if (ts < 0.030) env = std::exp (-std::fmod (ts, 0.010) / 0.003);
            else env = 0.5 * std::exp (-(ts - 0.030) / (0.10 * dec));
            y = v.f1.process (noise()) * env * 2.2;
            tail = 0.03 + 0.10 * dec;
            break;
        }
        case kTom:
        {
            const double f = 100.0 * tune * (1.0 + 0.6 * std::exp (-ts / 0.04));
            v.ph[0] += f / sr_;
            y = std::sin (kTwoPi * v.ph[0]) * std::exp (-ts / (0.25 * dec));
            tail = 0.25 * dec;
            break;
        }
        case kRim:
        {
            v.ph[0] += 1700.0 * tune / sr_;
            v.ph[1] += 460.0 * tune / sr_;
            y = (std::sin (kTwoPi * v.ph[0]) + 0.6 * std::sin (kTwoPi * v.ph[1])) * std::exp (-ts / (0.012 * dec)) * 0.5 + 0.2 * noise() * std::exp (-ts / 0.0005);
            tail = 0.012 * dec;
            break;
        }
        default:   // cloche
        {
            v.ph[0] += 540.0 * tune / sr_;
            v.ph[1] += 800.0 * tune / sr_;
            v.ph[0] -= std::floor (v.ph[0]);
            v.ph[1] -= std::floor (v.ph[1]);
            const double s = (v.ph[0] < 0.5 ? 1.0 : -1.0) + (v.ph[1] < 0.5 ? 1.0 : -1.0);
            y = v.f1.process (static_cast<float> (s)) * (0.6 * std::exp (-ts / 0.012) + 0.4) * std::exp (-ts / (0.25 * dec)) * 0.9;
            tail = 0.25 * dec;
            break;
        }
    }
    if (ts > 9.0 * std::max (tail, 0.002) + 0.04) v.active = false;
    return static_cast<float> (y) * v.velocity;
}

// ---------------------------------------------------------------------------------------------------------------- bloc
void GeneratorEngine::process (float* left, float* right, int n, const Transport& tr) noexcept
{
    if (n <= 0) return;
    const double sr = sr_;
    const int sig = std::clamp (static_cast<int> (std::lround (p (iSignal_))), 0, static_cast<int> (kNumSignals) - 1);
    const int md = std::clamp (static_cast<int> (std::lround (p (iMode_))), 0, static_cast<int> (kNumModes) - 1);

    // ---- événements de l'interface
    if (const auto c = panicCount_.load (std::memory_order_relaxed); c != lastPanicCount_)
    {
        lastPanicCount_ = c;
        gate_ = false;
        gain_ = 0.0f;
        heldCount_ = 0;
        for (auto& v : voices_) v.active = false;
    }
    if (const auto c = triggerCount_.load (std::memory_order_relaxed); c != lastTriggerCount_)
    {
        lastTriggerCount_ = c;
        if (md == kTriggered) startSignal (p (iFreq_), false);
    }
    for (std::size_t d = 0; d < static_cast<std::size_t> (kNumDrums); ++d)
        if (const auto c = drumCount_[d].load (std::memory_order_relaxed); c != lastDrumCount_[d]) { lastDrumCount_[d] = c; triggerDrum (static_cast<int> (d), 0.8f); }

    // ---- paramètres de signal
    const bool onNow = p (iOn_) >= 0.5;
    const bool trigNow = p (iTrigger_) >= 0.5;
    if (trigNow && ! lastTrigger_ && md == kTriggered) startSignal (p (iFreq_), false);
    lastTrigger_ = trigNow;
    if (md == kContinuous)
    {
        if (onNow && ! lastOn_) { tSig_ = 0.0; tBurst_ = 0.0; phase_ = sig == kImpulse ? 1.0 : 0.0; phase2_ = 0.0; remain_ = -1; noteMode_ = false; }
        gate_ = onNow;
    }
    else if (lastMode_ == kContinuous) gate_ = false;
    lastOn_ = onNow;
    lastMode_ = md;

    const bool isNoise = sig >= kWhite && sig <= kViolet;
    const bool isSweep = sig == kSweepLog || sig == kSweepLin;
    const double master = dbToLin (p (iMaster_));
    double amp = dbToLin (p (iLevel_));
    if (isNoise) amp *= noiseNorm()[static_cast<std::size_t> (sig - kWhite)];
    const double bal = p (iPan_) * 0.01;
    const double gLt = bal > 0.0 ? 1.0 - bal : 1.0, gRt = bal < 0.0 ? 1.0 + bal : 1.0;
    const int pol = static_cast<int> (std::lround (p (iPolarity_)));
    const double sgnL = (pol == kPolLeft || pol == kPolBoth) ? -1.0 : 1.0, sgnR = (pol == kPolRight || pol == kPolBoth) ? -1.0 : 1.0;
    const double targetL = amp * master * gLt * sgnL, targetR = amp * master * gRt * sgnR;
    const double pw = std::clamp (p (iWidth_) * 0.01, 0.01, 0.99);
    const double a2 = dbToLin (p (iLevel2_)), f2 = std::min (p (iFreq2_), 0.49 * sr);
    const double sweepT = std::max (0.05, p (iSweepTime_));
    const double sw0 = std::clamp (p (iSweepStart_), 5.0, 0.49 * sr), sw1 = std::clamp (p (iSweepEnd_), 5.0, 0.49 * sr);
    const long long burstOnS = static_cast<long long> (p (iBurstOn_) * 0.001 * sr), burstOffS = static_cast<long long> (p (iBurstOff_) * 0.001 * sr);
    const bool bursting = md == kContinuous && burstOnS > 0 && burstOffS > 0;
    const double fHz = std::min (noteMode_ ? noteFreq_ : p (iFreq_), 0.49 * sr);
    const float fadeStep = sig == kImpulse ? 1.0f : static_cast<float> (1.0 / (kFadeSeconds * sr));
    const long long fadeSamples = sig == kImpulse ? 0 : static_cast<long long> (kFadeSeconds * sr);
    const float k = static_cast<float> (1.0 - std::exp (-1.0 / (0.005 * sr)));
    if (! smInit_) { ampLSm_ = static_cast<float> (targetL); ampRSm_ = static_cast<float> (targetR); smInit_ = true; }

    // ---- paramètres de batterie
    const double drumBus = dbToLin (p (iDrumLevel_)) * master;
    DrumSnapshot ds {};
    for (std::size_t d = 0; d < static_cast<std::size_t> (kNumDrums); ++d)
    {
        const double b = p (iDrum_[d][kDrumPan]) * 0.01;
        ds.gain[d] = static_cast<float> (dbToLin (p (iDrum_[d][kDrumLevel])) * drumBus);
        ds.panL[d] = static_cast<float> (b > 0.0 ? 1.0 - b : 1.0);
        ds.panR[d] = static_cast<float> (b < 0.0 ? 1.0 + b : 1.0);
        ds.mute[d] = p (iDrum_[d][kDrumMute]) >= 0.5;
    }

    // ---- séquenceur
    const bool hostSync = static_cast<int> (std::lround (p (iSeqSync_))) == kSyncHost;
    const bool playing = hostSync ? (tr.valid && tr.playing) : p (iSeqRun_) >= 0.5;
    const double bpm = hostSync && tr.valid && tr.bpm > 1.0 ? tr.bpm : p (iTempo_);
    const int nsteps = std::clamp (static_cast<int> (std::lround (p (iSteps_))), 4, kMaxSteps);
    const double sw = p (iSwing_) * 0.005;   // retard des pas impairs, en fraction de pas : 0..0,375 (66 % : le triolet)
    double ppq0 = 0.0;
    if (playing)
    {
        if (hostSync) ppq0 = tr.ppq;
        else
        {
            if (! seqPlaying_) seqPpq_ = 0.0;
            ppq0 = seqPpq_;
        }
    }
    else step_.store (-1, std::memory_order_relaxed);
    seqPlaying_ = playing;
    const double ppqInc = bpm / 60.0 / sr;
    const double stepsPerBeat = nsteps / 4.0;
    long long lastCur = lastStepGlobal_;
    if (! playing) lastCur = std::numeric_limits<long long>::min();

    // ---- métronome et note de référence de l'accordeur : un tampon mono ajouté aux deux voies
    const int nAux = std::min (n, static_cast<int> (aux_.size()));
    {
        metronome::Settings ms;
        ms.run = p (iMetroOn_) >= 0.5;
        ms.hostSync = static_cast<int> (std::lround (p (iMetroSync_))) == kSyncHost;
        ms.bpm = p (iMetroTempo_);
        ms.beats = static_cast<int> (std::lround (p (iMetroBeats_)));
        const int unitChoice = static_cast<int> (std::lround (p (iMetroUnit_)));
        ms.unit = unitChoice == 1 ? 8 : unitChoice == 2 ? 16 : 4;
        ms.sub = static_cast<int> (std::lround (p (iMetroSub_))) + 1;
        ms.sound = static_cast<int> (std::lround (p (iMetroSound_)));
        ms.levelDb = p (iMetroLevel_);
        ms.accentDb = p (iMetroAccent_);
        ms.subDb = p (iMetroSubLevel_);
        metronome::HostPos hp;
        hp.valid = tr.valid;
        hp.playing = tr.playing;
        hp.ppq = tr.ppq;
        hp.bpm = tr.bpm;
        metro_.render (ms, hp, aux_.data(), nAux);
        const double midi = static_cast<double> (std::lround (p (iTunerNote_))) + p (iTunerCents_) * 0.01;
        tone_.render (aux_.data(), nAux, p (iTunerTone_) >= 0.5, tuner::noteFrequency (midi, p (iTunerA4_)), static_cast<float> (dbToLin (p (iTunerLevel_))));
        const float mg = static_cast<float> (master);
        for (int i = 0; i < nAux; ++i) aux_[static_cast<std::size_t> (i)] *= mg;   // le réglage général vaut aussi pour le clic et la note de référence
    }

    float peakL = 0.0f, peakR = 0.0f;
    bool anySound = false;
    for (int i = 0; i < n; ++i)
    {
        // ---- séquenceur : pas courant à cet échantillon
        if (playing)
        {
            const double pos = (ppq0 + i * ppqInc) * stepsPerBeat;
            const double fl = std::floor (pos);
            const long long base = static_cast<long long> (fl);
            const int idxBase = static_cast<int> (((base % nsteps) + nsteps) % nsteps);
            long long cur = base;
            if ((idxBase & 1) != 0 && pos - fl < sw) cur = base - 1;
            if (cur != lastCur)
            {
                lastCur = cur;
                const int idx = static_cast<int> (((cur % nsteps) + nsteps) % nsteps);
                step_.store (idx, std::memory_order_relaxed);
                for (std::size_t d = 0; d < static_cast<std::size_t> (kNumDrums); ++d)
                {
                    const int st = static_cast<int> (std::lround (p (iStep_[d][static_cast<std::size_t> (idx)])));
                    if (st > 0) triggerDrum (static_cast<int> (d), st >= 2 ? 1.0f : 0.7f);
                }
            }
        }

        // ---- signal
        bool g = gate_;
        if (g && bursting)
        {
            const long long period = burstOnS + burstOffS;
            const long long tb = static_cast<long long> (tBurst_);
            g = tb < std::max (burstOnS - fadeSamples, burstOnS / 2);   // le fondu de sortie finit avec la salve
            if (tb == 0) { tSig_ = 0.0; phase_ = sig == kImpulse ? 1.0 : 0.0; phase2_ = 0.0; }
            tBurst_ += 1.0;
            if (tBurst_ >= static_cast<double> (period)) tBurst_ = 0.0;
        }
        if (g && md == kTriggered && remain_ >= 0)
        {
            if (remain_ <= fadeSamples) g = false;
            if (--remain_ < 0) { gate_ = false; remain_ = -1; }
        }
        gain_ += g ? std::min (fadeStep, 1.0f - gain_) : -std::min (fadeStep, gain_);
        float s = 0.0f;
        if (gain_ > 0.0f)
        {
            anySound = true;
            double y = 0.0;
            const double dt = fHz / sr;
            switch (sig)
            {
                case kSine: y = std::sin (kTwoPi * phase_); break;
                case kSquare:
                {
                    y = phase_ < pw ? 1.0 : -1.0;
                    double t2 = phase_ - pw + 1.0;
                    t2 -= std::floor (t2);
                    y += polyBlep (phase_, dt) - polyBlep (t2, dt);
                    break;
                }
                case kTriangle: y = phase_ < 0.25 ? 4.0 * phase_ : phase_ < 0.75 ? 2.0 - 4.0 * phase_ : 4.0 * phase_ - 4.0; break;
                case kSawUp: y = 2.0 * phase_ - 1.0 - polyBlep (phase_, dt); break;
                case kSawDown: y = -(2.0 * phase_ - 1.0 - polyBlep (phase_, dt)); break;
                case kImpulse: y = phase_ >= 1.0 ? 1.0 : 0.0; if (y > 0.0) phase_ -= 1.0; break;
                case kTwoTone: y = (std::sin (kTwoPi * phase_) + a2 * std::sin (kTwoPi * phase2_)) / (1.0 + a2); break;
                case kDc: y = 1.0; break;
                case kSweepLog:
                case kSweepLin:
                {
                    double u = tSig_ / (sweepT * sr);
                    if (md == kContinuous) u -= std::floor (u);
                    else u = std::min (u, 1.0);
                    const double f = sig == kSweepLog ? sw0 * std::pow (sw1 / sw0, u) : sw0 + (sw1 - sw0) * u;
                    y = std::sin (kTwoPi * phase_);
                    phase_ += f / sr;
                    phase_ -= std::floor (phase_);
                    tSig_ += 1.0;
                    if (md == kContinuous && tSig_ >= sweepT * sr) tSig_ = 0.0;
                    sweepPos_.store (u, std::memory_order_relaxed);
                    break;
                }
                default: y = colouredNoise (sig - kWhite, rng_.white(), colour_); break;
            }
            if (! isSweep)
            {
                if (sig != kImpulse) { phase_ += dt; phase_ -= std::floor (phase_); }
                else phase_ += dt;
                if (sig == kTwoTone) { phase2_ += f2 / sr; phase2_ -= std::floor (phase2_); }
                tSig_ += 1.0;
            }
            s = static_cast<float> (y) * gain_;
        }
        ampLSm_ += k * (static_cast<float> (targetL) - ampLSm_);
        ampRSm_ += k * (static_cast<float> (targetR) - ampRSm_);
        float l = s * ampLSm_, r = s * ampRSm_;
        if (isNoise) { l = std::clamp (l, -1.0f, 1.0f); r = std::clamp (r, -1.0f, 1.0f); }

        // ---- batterie
        for (std::size_t d = 0; d < static_cast<std::size_t> (kNumDrums); ++d)
        {
            auto& v = voices_[d];
            if (! v.active) continue;
            const float y = renderDrum (static_cast<int> (d), v);
            if (ds.mute[d]) continue;
            l += y * ds.gain[d] * ds.panL[d];
            r += y * ds.gain[d] * ds.panR[d];
            anySound = true;
        }
        if (i < nAux) { const float a = aux_[static_cast<std::size_t> (i)]; l += a; r += a; if (a != 0.0f) anySound = true; }
        left[i] = l;
        right[i] = r;
        peakL = std::max (peakL, std::fabs (l));
        peakR = std::max (peakR, std::fabs (r));
    }
    lastStepGlobal_ = lastCur;
    if (playing && ! hostSync) seqPpq_ = ppq0 + n * ppqInc;
    peakL_.store (peakL, std::memory_order_relaxed);
    peakR_.store (peakR, std::memory_order_relaxed);
    sounding_.store (anySound && gain_ > 0.0f, std::memory_order_relaxed);
}
} // namespace neptune::core::generator
