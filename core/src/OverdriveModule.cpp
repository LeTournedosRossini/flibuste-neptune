#include "neptune/core/OverdriveModule.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace neptune::core
{
using namespace overdrive;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kSlopes[3] = { 12, 24, 48 };
constexpr int kFactors[3] = { 1, 2, 4 };

inline double dbToLin (double db) noexcept { return std::exp (db * 0.11512925464970229); }
// tanh par fraction continue (7e ordre) : erreur inférieure à 1e-4 sur +-5, bornée à +-1, monotone ; bien moins chère que std::tanh aux cadences hautes
inline double fastTanh (double x) noexcept
{
    const double u = std::clamp (x, -5.0, 5.0), u2 = u * u;
    const double y = u * (135135.0 + u2 * (17325.0 + u2 * (378.0 + u2))) / (135135.0 + u2 * (62370.0 + u2 * (3150.0 + 28.0 * u2)));
    return std::clamp (y, -1.0, 1.0);
}
inline double smoothCoef (double seconds, double fs) noexcept { return 1.0 - std::exp (-1.0 / (std::max (seconds, 1.0e-4) * fs)); }
inline int choiceIndex (double v, int count) noexcept { return std::clamp (static_cast<int> (v + 0.5), 0, count - 1); }

// ---- courbes : toutes de pente 1 à l'origine et bornées près de +-1
inline double softRat (double u) noexcept { return u / (1.0 + std::fabs (u)); }
inline double hardClip (double u) noexcept { return std::clamp (u, -1.0, 1.0); }
// Famille de coudes : 0 très doux (rationnelle), 0,5 tangente hyperbolique, 1 écrêtage dur.
inline double knee (double u, double c) noexcept
{
    if (c < 0.5)
    {
        const double t = c * 2.0;
        return (1.0 - t) * softRat (u) + t * fastTanh (u);
    }
    const double t = (c - 0.5) * 2.0;
    return (1.0 - t) * fastTanh (u) + t * hardClip (u);
}
// Étage de triode : le point de repos (bias) décale l'entrée ; on retire la valeur au repos pour que zéro reste zéro.
inline double tubeStage (double x, double b, double c) noexcept { return knee (x + b, c) - knee (b, c); }
} // namespace

namespace overdrive
{
const char* modelName (int m) noexcept
{
    switch (m)
    {
        case kTape: return "Tape";
        case kTransistor: return "Transistor";
        case kFuzz: return "Fuzz";
        default: return "Tube";
    }
}

namespace
{
// Étages élémentaires (utilisés par la courbe statique et par HiStage). Le caractère d'une lampe est plafonné à 0,7 : une lampe n'écrête jamais « dur ».
inline double tubeCharacter (double c) noexcept { return 0.7 * c; }
inline double diodeStage (double x, const ShapeParams& p) noexcept
{
    // diodes : exponentielle de chaque côté, limites différentes selon l'asymétrie ; un peu d'écrêtage dur seulement quand le caractère est poussé
    const double kp = std::exp (0.7 * p.bias), kn = std::exp (-0.7 * p.bias);
    const double y = x >= 0.0 ? (1.0 - std::exp (-x * kp)) / kp : -(1.0 - std::exp (x * kn)) / kn;
    return (1.0 - 0.35 * p.character) * y + 0.35 * p.character * hardClip (x);
}
inline double fuzzStage (double x, const ShapeParams& p) noexcept
{
    // polarisation décalée, porte qui étouffe les petits signaux, écrêtage de plus en plus carré
    const double g = 0.12 * p.feel, ax = std::fabs (x);
    const double gate = (g > 1.0e-6 && ax < g) ? (ax / g) * (ax / g) : 1.0;
    const double u = 2.0 * (x + 0.5 * p.bias);
    return gate * ((1.0 - p.character) * fastTanh (u) + p.character * hardClip (u));
}
inline double onePoleCoef (double fc, double fs) noexcept { return 1.0 - std::exp (-2.0 * kPi * std::min (fc, 0.45 * fs) / fs); }
} // namespace

double shape (double x, const ShapeParams& p) noexcept
{
    switch (p.model)
    {
        case kTape:
            return knee (x, p.character * 0.6);   // symétrique, jamais très dur
        case kTransistor:
            return diodeStage (0.8 * diodeStage (x, p), p);   // deux étages à diodes (le second un peu moins poussé)
        case kFuzz:
            return fuzzStage (x, p);
        default:
        {
            // lampe : deux étages de triode en cascade, point de repos opposé et plus faible pour le second
            const double b = 0.6 * p.bias, c = tubeCharacter (p.character);
            const double y1 = tubeStage (x, b, c);
            return tubeStage (1.2 * y1, -0.4 * b, c);
        }
    }
}

void HiStage::configure (double fsHi, int model, double driveDb, double smooth01) noexcept
{
    model_ = model;
    const double drive01 = std::clamp (driveDb / 60.0, 0.0, 1.0);
    static const double postBase[kNumModels] = { 7000.0, 9000.0, 5500.0, 4500.0 };   // passe-bas après l'écrêteur : lampe, bande, transistor, fuzz
    const int m = std::clamp (model, 0, kNumModels - 1);
    const double scale = 1.5 - 1.1 * std::clamp (smooth01, 0.0, 1.0);   // douceur : de x1,5 (clair) à x0,4 (très adouci)
    aPost_ = onePoleCoef (postBase[m] * scale * (1.0 - 0.4 * drive01), fsHi);   // plus on pousse, plus on adoucit
    aCouple_ = onePoleCoef (model == kTransistor ? 40.0 : 30.0, fsHi);   // condensateur de liaison
    aMid_ = onePoleCoef (model == kTransistor ? 8000.0 : 9000.0, fsHi);   // passe-bas entre les deux étages
}

double HiStage::process (double x, const ShapeParams& p) noexcept
{
    double y;
    switch (model_)
    {
        case kTape:
            y = knee (x, p.character * 0.6);
            break;
        case kFuzz:
            y = fuzzStage (x, p);
            break;
        case kTransistor:
        {
            double y1 = diodeStage (x, p);
            couple_ += aCouple_ * (y1 - couple_);   // la polarisation se recentre : le continu créé par l'asymétrie est retiré lentement
            y1 -= couple_;
            mid_ += aMid_ * (y1 - mid_);
            y = diodeStage (0.8 * mid_, p);
            break;
        }
        default:
        {
            const double b = 0.6 * p.bias, c = tubeCharacter (p.character);
            double y1 = tubeStage (x, b, c);
            couple_ += aCouple_ * (y1 - couple_);
            y1 -= couple_;
            mid_ += aMid_ * (y1 - mid_);
            y = tubeStage (1.2 * mid_, -0.4 * b, c);
            break;
        }
    }
    post1_ += aPost_ * (y - post1_);       // passe-bas à deux pôles
    post2_ += aPost_ * (post1_ - post2_);
    return post2_;
}

double autoGainFor (double driveLin, const ShapeParams& p) noexcept
{
    // On fait passer un sinus de -12 dBFS (480 Hz, 200 échantillons par période à 96 kHz) par l'étage complet, filtres compris.
    constexpr double fsHi = 96000.0, amp = 0.25;
    constexpr int settle = 8192, measure = 4000;
    HiStage st;
    st.configure (fsHi, p.model, 20.0 * std::log10 (std::max (driveLin, 1.0)), p.smooth);
    double sum = 0.0, mean = 0.0;
    std::array<double, measure> ys {};
    for (int i = 0; i < settle + measure; ++i)
    {
        const double y = st.process (driveLin * amp * std::sin (2.0 * kPi * 480.0 * static_cast<double> (i) / fsHi), p);
        if (i >= settle) { ys[static_cast<std::size_t> (i - settle)] = y; mean += y; }
    }
    mean /= measure;
    for (const double y : ys) sum += (y - mean) * (y - mean);
    const double rmsOut = std::sqrt (sum / measure), rmsIn = amp / std::sqrt (2.0);
    if (rmsOut < 1.0e-9) return 1.0;
    return std::clamp (rmsIn / rmsOut, 0.05, 20.0);
}
} // namespace overdrive

// ---- module
void OverdriveModule::Chain::reset() noexcept
{
    os.reset();
    inHp.reset(); inLp.reset(); outHp.reset(); outLp.reset();
    hi.reset();
    gbw = 0.0;
    pre.reset(); post1.reset(); post2.reset(); dc.reset();
    toneLp = sagFast = sagSlow = biasDyn = lowLp = lowLp2 = 0.0;
    lastGainArg = 1.0e9;
    driftTick = 0;
    driftPhase1 = index == 0 ? 0.3 : 2.2;   // départs fixes : le rendu est déterministe
    driftPhase2 = index == 0 ? 1.7 : 4.1;
    lowRing.fill (0.0f);
    lowPos = 0;
    delay.fill (0.0f);
    delayPos = 0;
}

OverdriveModule::OverdriveModule()
{
    idx_.fill (ParameterSet::npos);
    for (auto& m : meters_) m.store (0.0f, std::memory_order_relaxed);
    hold_.fill (0.0f);
}

bool OverdriveModule::declareParams (ParameterSet& set, std::string* error) const
{
    using P = ParamSpec;
    bool ok = true;
    auto add = [&] (P s) { ok = ok && set.add (std::move (s), error); };
    auto fixed = [] (P s) { s.automatable = false; return s; };
    auto pct = [] (ParamId id, const char* n, double lo, double hi, double def) { return P::continuous (id, n, Unit::Percent, lo, hi, def); };
    const std::vector<std::string> slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };

    add (P::choice (model, "Model", { "Tube", "Tape", "Transistor", "Fuzz" }, kTube));
    add (P::continuous (drive, "Drive", Unit::Decibel, 0.0, 60.0, 18.0));
    add (pct (bias, "Bias", -100.0, 100.0, 0.0));
    add (pct (character, "Character", 0.0, 100.0, 50.0));
    add (pct (feel, "Feel", 0.0, 100.0, 40.0));
    add (pct (tone, "Tone", -100.0, 100.0, 0.0));
    add (P::continuous (level, "Output Level", Unit::Decibel, -24.0, 12.0, 0.0));
    add (P::toggle (autoGain, "Auto Gain", true));
    add (fixed (P::choice (oversampling, "Oversampling", { "Off", "2x", "4x" }, 1)));
    add (fixed (P::choice (channels, "Channels", { "Stereo", "Mid/Side", "Mid only", "Side only" }, kStereo)));

    add (P::toggle (inHpOn, "Input HP On"));
    add (P::continuous (inHp, "Input HP", Unit::Hertz, 20.0, 1000.0, 80.0, Curve::Exponential));
    add (fixed (P::choice (inHpSlope, "Input HP Slope", slopes, 0)));
    add (P::toggle (inLpOn, "Input LP On"));
    add (P::continuous (inLp, "Input LP", Unit::Hertz, 1000.0, 20000.0, 8000.0, Curve::Exponential));
    add (fixed (P::choice (inLpSlope, "Input LP Slope", slopes, 0)));
    add (P::toggle (outHpOn, "Output HP On"));
    add (P::continuous (outHp, "Output HP", Unit::Hertz, 20.0, 1000.0, 40.0, Curve::Exponential));
    add (fixed (P::choice (outHpSlope, "Output HP Slope", slopes, 0)));
    add (P::toggle (outLpOn, "Output LP On"));
    add (P::continuous (outLp, "Output LP", Unit::Hertz, 1000.0, 20000.0, 12000.0, Curve::Exponential));
    add (fixed (P::choice (outLpSlope, "Output LP Slope", slopes, 0)));

    add (P::toggle (eqOn, "EQ On", true));
    const double defFreq[3] = { 100.0, 1000.0, 6000.0 };
    const int defType[3] = { 1, 0, 2 };   // plateau grave, cloche, plateau aigu
    for (int b = 0; b < 3; ++b)
    {
        const std::string n = "EQ " + std::to_string (b + 1);
        add (P::toggle (bandParam (b, kOn), n + " On"));
        add (P::choice (bandParam (b, kType), n + " Type", { "Bell", "Low Shelf", "High Shelf" }, defType[b]));
        add (P::continuous (bandParam (b, kFreq), n + " Freq", Unit::Hertz, 20.0, 20000.0, defFreq[b], Curve::Exponential));
        add (P::continuous (bandParam (b, kGain), n + " Gain", Unit::Decibel, -18.0, 18.0, 0.0));
        add (P::continuous (bandParam (b, kQ), n + " Q", Unit::None, 0.1, 10.0, 1.0, Curve::Exponential));
    }
    add (P::toggle (eqHpOn, "EQ HP On"));
    add (P::continuous (eqHp, "EQ HP", Unit::Hertz, 20.0, 1000.0, 80.0, Curve::Exponential));
    add (fixed (P::choice (eqHpSlope, "EQ HP Slope", slopes, 0)));
    add (P::toggle (eqLpOn, "EQ LP On"));
    add (P::continuous (eqLp, "EQ LP", Unit::Hertz, 1000.0, 20000.0, 12000.0, Curve::Exponential));
    add (fixed (P::choice (eqLpSlope, "EQ LP Slope", slopes, 0)));

    add (fixed (P::choice (keySource, "Key Source", { "Off", "Main", "Sidechain" }, kKeyOff)));
    add (pct (keyAmount, "Key Amount", -100.0, 100.0, 0.0));
    add (P::continuous (keyAttack, "Key Attack", Unit::Milliseconds, 1.0, 200.0, 5.0, Curve::Exponential));
    add (P::continuous (keyRelease, "Key Release", Unit::Milliseconds, 10.0, 1000.0, 120.0, Curve::Exponential));
    add (P::continuous (keyHp, "Key HP", Unit::Hertz, 20.0, 2000.0, 20.0, Curve::Exponential));
    add (pct (smooth, "Smooth", 0.0, 100.0, 60.0));
    add (pct (tight, "Tight", 0.0, 100.0, 0.0));
    add (pct (imperfection, "Imperfection", 0.0, 100.0, 25.0));
    return ok;
}

void OverdriveModule::bind (const ParameterSet& set)
{
    for (std::size_t i = 0; i < idx_.size(); ++i) idx_[i] = set.indexOf (static_cast<ParamId> (100 + i));
}

void OverdriveModule::prepare (double sampleRate, int maxBlockSize)
{
    fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max (1, maxBlockSize);
    for (std::size_t i = 0; i < chain_.size(); ++i)
    {
        chain_[i].index = static_cast<int> (i);
        chain_[i].os.prepare (maxBlock_);
        chain_[i].low.assign (static_cast<std::size_t> (maxBlock_), 0.0f);
    }
    for (std::size_t i = 0; i < old_.size(); ++i)
    {
        old_[i].os.prepare (maxBlock_);
        old_[i].low.assign (static_cast<std::size_t> (maxBlock_), 0.0f);
        old_[i].dynBuf.assign (static_cast<std::size_t> (maxBlock_), 0.0f);
        chain_[i].dynBuf.assign (static_cast<std::size_t> (maxBlock_), 0.0f);
    }
    for (auto* v : { &a_, &b_, &keepA_, &keepB_, &modDb_, &oa_, &ob_ }) v->assign (static_cast<std::size_t> (maxBlock_), 0.0f);
    eq_.prepare (fs_);
    sagAttackF_ = smoothCoef (0.003, fs_);   // affaissement : une composante rapide (condensateur de filtrage de l'étage) ...
    sagReleaseF_ = smoothCoef (0.060, fs_);
    sagAttackS_ = smoothCoef (0.025, fs_);   // ... et une lente (alimentation)
    sagReleaseS_ = smoothCoef (0.450, fs_);
    lowCoef_ = 1.0 - std::exp (-2.0 * kPi * 180.0 / fs_);   // Tight : séparation des graves à 180 Hz
    driftInc1_ = 2.0 * kPi * 0.13 / fs_;                     // Imperfection : dérives lentes (0,13 Hz et 0,31 Hz)
    driftInc2_ = 2.0 * kPi * 0.31 / fs_;
    gainSmooth_ = smoothCoef (0.005, fs_);
    levelSmooth_ = smoothCoef (0.010, fs_);
    toneCoef_ = 1.0 - std::exp (-2.0 * kPi * 800.0 / fs_);
    haveLast_ = false;
    reset();
}

void OverdriveModule::reset() noexcept
{
    for (auto& c : chain_) { c.reset(); c.gain = 1.0; }
    eq_.reset();
    keyHpFilter_.reset();
    keyEnv_ = 0.0;
    fadeLeft_ = 0;
    smoothInit_ = false;
    hold_.fill (0.0f);
}

int OverdriveModule::latencySamples (const ParameterSet& params) const noexcept
{
    const auto i = params.indexOf (oversampling);
    const int f = zero_.load (std::memory_order_relaxed) ? 1 : i == ParameterSet::npos ? 2 : kFactors[choiceIndex (params.getAt (i), 3)];
    return Oversampler::latencyFor (f);
}

void OverdriveModule::readParams (const ParameterSet& params) noexcept
{
    for (std::size_t i = 0; i < v_.size(); ++i) v_[i] = idx_[i] != ParameterSet::npos ? params.getAt (idx_[i]) : 0.0;
}

namespace
{
// Réglages de l'égaliseur à partir des paramètres (`at` : identifiant -> valeur)
template <class At>
llc::EQParams eqParamsFrom (At at)
{
    llc::EQParams eq;
    for (int b = 0; b < 3; ++b)
    {
        auto& band = eq.band[b];
        band.enabled = at (bandParam (b, kOn)) >= 0.5;
        band.type = static_cast<llc::BandType> (choiceIndex (at (bandParam (b, kType)), 3));
        band.freq = at (bandParam (b, kFreq));
        band.gainDb = at (bandParam (b, kGain));
        band.q = at (bandParam (b, kQ));
    }
    eq.hp = { at (eqHpOn) >= 0.5, at (eqHp), kSlopes[choiceIndex (at (eqHpSlope), 3)] };
    eq.lp = { at (eqLpOn) >= 0.5, at (eqLp), kSlopes[choiceIndex (at (eqLpSlope), 3)] };
    return eq;
}
} // namespace

llc::EQParams OverdriveModule::makeEq() const noexcept
{
    return eqParamsFrom ([this] (ParamId id) { return param (id); });
}

double OverdriveModule::eqResponseDb (const ParameterSet& set, double freq, double sampleRate)
{
    if (set.get (eqOn) < 0.5) return 0.0;
    return llc::ParametricEQ::responseDb (eqParamsFrom ([&set] (ParamId id) { return set.get (id); }), freq, sampleRate);
}

double OverdriveModule::eqBandResponseDb (const ParameterSet& set, int band, double freq, double sampleRate)
{
    if (set.get (eqOn) < 0.5) return 0.0;
    const auto eq = eqParamsFrom ([&set] (ParamId id) { return set.get (id); });
    if (band >= 0 && band < 3) return llc::ParametricEQ::bandResponseDb (eq.band[band], freq, sampleRate);
    if (band == 3) return llc::ParametricEQ::passResponseDb (eq.hp, true, freq, sampleRate);
    if (band == 4) return llc::ParametricEQ::passResponseDb (eq.lp, false, freq, sampleRate);
    return 0.0;
}

void OverdriveModule::updateDesigns() noexcept
{
    const bool snap = ! haveLast_;
    last_ = v_;
    haveLast_ = true;

    modelIndex_ = choiceIndex (param (model), kNumModels);
    factor_ = zero_.load (std::memory_order_relaxed) ? 1 : kFactors[choiceIndex (param (oversampling), 3)];   // profil Live : pas de suréchantillonnage
    channelMode_ = choiceIndex (param (channels), kNumChannelModes);
    keyMode_ = choiceIndex (param (keySource), kNumKeySources);
    latency_ = Oversampler::latencyFor (factor_);
    if (! snap && chain_[0].os.factor() != factor_)
    {
        // le retard du signal change d'un coup (0, 31 ou 39 échantillons) : on garde l'ancienne chaîne et on fait un fondu enchaîné vers la nouvelle
        for (std::size_t i = 0; i < chain_.size(); ++i) old_[i] = chain_[i];
        fadeLeft_ = kFadeLen;
    }
    for (auto& c : chain_)
        if (c.os.factor() != factor_) c.os.setFactor (factor_);

    driveDb_ = param (drive);
    shapeTarget_ = { modelIndex_, param (bias) / 100.0, param (character) / 100.0, param (feel) / 100.0, param (smooth) / 100.0 };
    compTarget_ = param (autoGain) >= 0.5 ? autoGainFor (dbToLin (driveDb_), shapeTarget_) : 1.0;
    levelLin_ = dbToLin (param (level));
    tight_ = param (tight) / 100.0;
    imperfection_ = param (imperfection) / 100.0;
    lowBack_ = param (autoGain) >= 0.5 ? 1.0 / std::max (compTarget_, 0.05) : 1.0;   // le niveau de sortie (avec compensation) s'applique aussi aux graves rajoutés : on le leur annule
    const double t = param (tone) / 100.0;
    toneGainLow_ = dbToLin (-6.0 * t);
    toneGainHigh_ = dbToLin (6.0 * t);

    const double feel01 = param (feel) / 100.0;
    BiquadCoefs pre, post1, post2;   // identité par défaut
    if (modelIndex_ == kTape)
    {
        pre = BiquadCoefs::highShelf (fs_, 4000.0, 8.0 * feel01, 0.707);          // les aigus saturent davantage ...
        post1 = BiquadCoefs::highShelf (fs_, 4000.0, -8.0 * feel01, 0.707);       // ... puis retour à la balance d'origine
        post2 = BiquadCoefs::lowShelf (fs_, 80.0, 3.0 * feel01, 0.707);           // bosse de la tête de lecture
    }
    else if (modelIndex_ == kTransistor)
        pre = BiquadCoefs::peaking (fs_, 800.0, 8.0 * feel01, 0.8);               // bosse des médiums avant l'écrêtage
    const auto dcBlock = BiquadCoefs::highpass (fs_, 10.0, 0.7071);
    for (auto& c : chain_)
    {
        c.inHp.design (true, fs_, param (inHp), kSlopes[choiceIndex (param (inHpSlope), 3)]);
        c.inLp.design (false, fs_, param (inLp), kSlopes[choiceIndex (param (inLpSlope), 3)]);
        c.outHp.design (true, fs_, param (outHp), kSlopes[choiceIndex (param (outHpSlope), 3)]);
        c.outLp.design (false, fs_, param (outLp), kSlopes[choiceIndex (param (outLpSlope), 3)]);
        c.pre.set (pre);
        c.post1.set (post1);
        c.post2.set (post2);
        c.dc.set (dcBlock);
    }
    keyHpFilter_.set (BiquadCoefs::highpass (fs_, param (keyHp), 0.7071));
    keyAttack_ = smoothCoef (param (keyAttack) * 0.001, fs_);
    keyRelease_ = smoothCoef (param (keyRelease) * 0.001, fs_);
    eq_.setParams (makeEq(), snap);
    if (snap) { shape_ = shapeTarget_; smoothInit_ = false; }
}

void OverdriveModule::delayThrough (Chain& c, float* buf, int len, int samples) noexcept
{
    const std::size_t d = static_cast<std::size_t> (std::clamp (samples, 0, 127));
    for (int i = 0; i < len; ++i)
    {
        c.delay[c.delayPos] = buf[i];
        buf[i] = c.delay[(c.delayPos + 128 - d) & 127];
        c.delayPos = (c.delayPos + 1) & 127;
    }
}

void OverdriveModule::processChain (Chain& c, float* buf, int len) noexcept
{
    const bool inHpOnNow = param (inHpOn) >= 0.5, inLpOnNow = param (inLpOn) >= 0.5, outHpOnNow = param (outHpOn) >= 0.5, outLpOnNow = param (outLpOn) >= 0.5;
    const bool isTube = modelIndex_ == kTube;
    const double sagAmount = isTube ? 1.5 * param (feel) / 100.0 : 0.0;
    const bool emphasis = modelIndex_ == kTape || modelIndex_ == kTransistor;
    const bool useTight = tight_ > 0.0005;
    const int factor = c.os.factor();   // la chaîne garde son propre facteur : l'ancienne chaîne d'un fondu enchaîné tourne encore à l'ancien
    const std::size_t lowDelay = static_cast<std::size_t> (std::clamp (Oversampler::latencyFor (factor), 0, 127));
    const double sign = c.index == 0 ? 1.0 : -1.0;
    const double feel01 = param (feel) / 100.0;

    // bande passante du gain : produit gain x bande passante constant par modèle, borné entre 2,5 kHz et 20 kHz (au-delà, le filtre est neutre)
    static const double gbwHz[kNumModels] = { 700000.0, 1500000.0, 400000.0, 300000.0 };

    // ---- cadence de base : filtres d'entrée, séparation des graves (Tight), préaccentuation, drive
    for (int i = 0; i < len; ++i)
    {
        double x = buf[i];
        if (! std::isfinite (x)) x = 0.0;
        if (inHpOnNow) x = c.inHp.process (static_cast<float> (x));
        if (inLpOnNow) x = c.inLp.process (static_cast<float> (x));
        if (useTight)
        {
            // x = passe-haut du second ordre (x - 2 LP1 + LP2) + graves (2 LP1 - LP2) : la distorsion ne reçoit que le passe-haut, plus ou moins selon Tight
            c.lowLp += lowCoef_ * (x - c.lowLp);
            c.lowLp2 += lowCoef_ * (c.lowLp - c.lowLp2);
            const double lowPart = tight_ * (2.0 * c.lowLp - c.lowLp2);   // la part des graves retirée de la distorsion
            c.low[static_cast<std::size_t> (i)] = static_cast<float> (lowPart);
            x -= lowPart;
        }
        if (emphasis) x = c.pre.processD (x);
        // imperfection : décalage fixe de signe opposé entre les deux voies, plus une dérive lente (gain en dB, polarisation) ; les phases avancent
        // à chaque échantillon et la dérive est recalculée tous les 16 échantillons : le résultat ne dépend pas de la taille des blocs
        c.driftPhase1 += driftInc1_;
        c.driftPhase2 += driftInc2_;
        if ((c.driftTick++ & 15) == 0)
        {
            const double driftA = 0.6 * std::sin (c.driftPhase1) + 0.4 * std::sin (c.driftPhase2);
            const double driftB = std::sin (c.driftPhase2 * 1.7 + 0.9);
            c.impDriveDb = imperfection_ * (0.45 * sign + 0.30 * driftA);
            c.impBias = imperfection_ * (0.06 * sign + 0.04 * driftB);
            const double fcGain = std::clamp (gbwHz[std::clamp (modelIndex_, 0, kNumModels - 1)] / std::max (c.gain, 1.0), 2500.0, 20000.0);
            c.gbwCoef = fcGain >= 19999.0 ? 1.0 : 1.0 - std::exp (-2.0 * kPi * fcGain / fs_);
        }
        const double gainArg = driveDb_ + c.impDriveDb + static_cast<double> (modDb_[static_cast<std::size_t> (i)]);
        if (std::fabs (gainArg - c.lastGainArg) > 1.0e-7) { c.lastGainArg = gainArg; c.lastGainTarget = dbToLin (gainArg); }
        const double target = c.lastGainTarget;
        if (! smoothInit_) c.gain = target;
        c.gain += (target - c.gain) * gainSmooth_;
        double g = c.gain;
        if (sagAmount > 0.0)
        {
            // affaissement : un signal fort tire l'alimentation vers le bas, le gain baisse (deux constantes de temps, rapide et lente)
            const double level01 = std::min (std::fabs (x * g), 4.0) * 0.25;
            c.sagFast += (level01 > c.sagFast ? sagAttackF_ : sagReleaseF_) * (level01 - c.sagFast);
            c.sagSlow += (level01 > c.sagSlow ? sagAttackS_ : sagReleaseS_) * (level01 - c.sagSlow);
            g /= 1.0 + sagAmount * (0.6 * c.sagFast + 0.4 * c.sagSlow);
        }
        c.dynBuf[static_cast<std::size_t> (i)] = static_cast<float> ((isTube ? 0.35 * feel01 * c.sagSlow : 0.0) + c.impBias);   // polarisation : dynamique (enveloppe lente) et imperfection
        const double driven = x * g;
        // gain-bande passante limitée (comme un amplificateur opérationnel) : plus le gain est fort, plus les aigus sont coupés AVANT l'écrêteur
        if (c.gbwCoef < 0.999) { c.gbw += c.gbwCoef * (driven - c.gbw); buf[i] = static_cast<float> (c.gbw); }
        else { c.gbw = driven; buf[i] = static_cast<float> (driven); }
    }

    // ---- cadence haute : courbe de transfert (réglages interpolés sur le bloc ; la polarisation de la lampe suit l'enveloppe lente du signal)
    float* hi = c.os.up (buf, len);
    const int total = len * factor;
    ShapeParams sp = shape_;
    c.hi.configure (fs_ * static_cast<double> (factor), modelIndex_, driveDb_, shapeTarget_.smooth);
    const double inv = 1.0 / static_cast<double> (std::max (1, total));
    const double stepBias = (shapeTarget_.bias - sp.bias) * inv, stepChar = (shapeTarget_.character - sp.character) * inv, stepFeel = (shapeTarget_.feel - sp.feel) * inv;
    for (int i = 0, k = 0; i < len; ++i)
    {
        const double dyn = static_cast<double> (c.dynBuf[static_cast<std::size_t> (i)]);
        for (int f = 0; f < factor; ++f, ++k)
        {
            sp.bias += stepBias;
            sp.character += stepChar;
            sp.feel += stepFeel;
            ShapeParams now = sp;
            now.bias += dyn;
            hi[k] = static_cast<float> (c.hi.process (static_cast<double> (hi[k]), now));
        }
    }
    c.os.down (buf, len);

    // ---- cadence de base : continu, tonalité, désaccentuation, graves propres (retardés comme la voie traitée), filtres de sortie
    for (int i = 0; i < len; ++i)
    {
        double y = c.dc.processD (static_cast<double>(buf[i]));
        c.toneLp += toneCoef_ * (y - c.toneLp);
        y = c.toneLp * toneGainLow_ + (y - c.toneLp) * toneGainHigh_;
        if (modelIndex_ == kTape) y = c.post2.processD (c.post1.processD (y));
        if (useTight)
        {
            c.lowRing[c.lowPos] = c.low[static_cast<std::size_t> (i)];
            y += lowBack_ * static_cast<double> (c.lowRing[(c.lowPos + 128 - lowDelay) & 127]);
            c.lowPos = (c.lowPos + 1) & 127;
        }
        float f = static_cast<float> (y);
        if (outHpOnNow) f = c.outHp.process (f);
        if (outLpOnNow) f = c.outLp.process (f);
        buf[i] = f;
    }
}

void OverdriveModule::processChunk (float* l, float* r, int len, const float* kl, const float* kr) noexcept
{
    float* a = a_.data();
    float* b = b_.data();
    auto hold = [this] (int m) -> float& { return hold_[static_cast<std::size_t> (m)]; };
    const bool firstBlock = ! smoothInit_;
    for (int i = 0; i < len; ++i)
    {
        hold (kInPeakL) = std::max (hold (kInPeakL), std::fabs (l[i]));
        hold (kInPeakR) = std::max (hold (kInPeakR), std::fabs (r[i]));
    }

    // ---- entrée latérale : enveloppe de la clé -> modulation du drive (en dB)
    if (keyMode_ != kKeyOff)
    {
        const double amount = param (keyAmount) / 100.0 * 30.0;
        for (int i = 0; i < len; ++i)
        {
            float k = 0.0f;
            if (keyMode_ == kKeyMain) k = 0.5f * (l[i] + r[i]);
            else if (kl != nullptr && kr != nullptr) k = 0.5f * (kl[i] + kr[i]);
            const double a2 = std::fabs (keyHpFilter_.processD (static_cast<double> (k)));
            keyEnv_ += (a2 > keyEnv_ ? keyAttack_ : keyRelease_) * (a2 - keyEnv_);
            const double db = 20.0 * std::log10 (std::max (keyEnv_, 1.0e-6));
            const double k01 = std::clamp ((db + 40.0) / 40.0, 0.0, 1.0);
            modDb_[static_cast<std::size_t> (i)] = static_cast<float> (amount * k01);
            hold (kKeyLevel) = std::max (hold (kKeyLevel), static_cast<float> (k01));
        }
    }
    else std::fill (modDb_.begin(), modDb_.begin() + len, 0.0f);

    // ---- voies : stéréo ou milieu / côtés
    const bool midSide = channelMode_ != kStereo;
    for (int i = 0; i < len; ++i)
    {
        if (midSide) { a[i] = 0.5f * (l[i] + r[i]); b[i] = 0.5f * (l[i] - r[i]); }
        else { a[i] = l[i]; b[i] = r[i]; }
    }
    const bool doA = channelMode_ != kSideOnly, doB = channelMode_ != kMidOnly;

    shape_.model = shapeTarget_.model;
    const bool fading = fadeLeft_ > 0;
    if (fading)
    {
        // changement de suréchantillonnage : l'ancienne chaîne traite le même signal, puis fondu enchaîné vers la nouvelle
        std::copy (a, a + len, oa_.begin());
        std::copy (b, b + len, ob_.begin());
        if (doA) processChain (old_[0], oa_.data(), len); else delayThrough (old_[0], oa_.data(), len, Oversampler::latencyFor (old_[0].os.factor()));
        if (doB) processChain (old_[1], ob_.data(), len); else delayThrough (old_[1], ob_.data(), len, Oversampler::latencyFor (old_[1].os.factor()));
    }
    if (doA) processChain (chain_[0], a, len); else delayThrough (chain_[0], a, len, latency_);
    if (doB) processChain (chain_[1], b, len); else delayThrough (chain_[1], b, len, latency_);
    shape_ = shapeTarget_;
    if (fading)
    {
        for (int i = 0; i < len; ++i)
        {
            const float t = i >= fadeLeft_ ? 1.0f : 1.0f - static_cast<float> (fadeLeft_ - i) / static_cast<float> (kFadeLen);
            a[i] = oa_[static_cast<std::size_t> (i)] * (1.0f - t) + a[i] * t;
            b[i] = ob_[static_cast<std::size_t> (i)] * (1.0f - t) + b[i] * t;
        }
        fadeLeft_ = std::max (0, fadeLeft_ - len);
    }

    // ---- égaliseur (la voie qui n'est pas traitée est rendue telle quelle)
    if (param (eqOn) >= 0.5)
    {
        if (! doA) std::copy (a, a + len, keepA_.begin());
        if (! doB) std::copy (b, b + len, keepB_.begin());
        eq_.process (a, b, len);
        if (! doA) std::copy (keepA_.begin(), keepA_.begin() + len, a);
        if (! doB) std::copy (keepB_.begin(), keepB_.begin() + len, b);
    }

    // ---- niveau de sortie (avec la compensation de gain), lissé ; seules les voies traitées le reçoivent
    const double gainTarget = levelLin_ * compTarget_;
    for (int i = 0; i < len; ++i)
    {
        if (firstBlock && i == 0) levelCur_ = gainTarget;
        levelCur_ += (gainTarget - levelCur_) * levelSmooth_;
        const float g = static_cast<float> (levelCur_);
        if (doA) a[i] *= g;
        if (doB) b[i] *= g;
        if (midSide) { l[i] = a[i] + b[i]; r[i] = a[i] - b[i]; }
        else { l[i] = a[i]; r[i] = b[i]; }
        hold (kOutPeakL) = std::max (hold (kOutPeakL), std::fabs (l[i]));
        hold (kOutPeakR) = std::max (hold (kOutPeakR), std::fabs (r[i]));
    }
    smoothInit_ = true;
}

void OverdriveModule::process (float* left, float* right, int n, const ParameterSet& params) noexcept
{
    if (n <= 0) return;
    readParams (params);
    if (! haveLast_ || v_ != last_) updateDesigns();
    const float* kl = keyL_;
    const float* kr = keyR_;
    keyL_ = keyR_ = nullptr;
    for (int done = 0; done < n;)
    {
        const int len = std::min (n - done, maxBlock_);
        processChunk (left + done, right + done, len, kl != nullptr ? kl + done : nullptr, kr != nullptr ? kr + done : nullptr);
        done += len;
    }
    for (std::size_t i = 0; i < hold_.size(); ++i)
    {
        meters_[i].store (hold_[i], std::memory_order_relaxed);
        hold_[i] = 0.0f;
    }
}
} // namespace neptune::core
