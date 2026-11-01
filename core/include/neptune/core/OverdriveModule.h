#pragma once
// « Fournaise » : overdrive et saturation à quatre modèles (lampe, bande, transistor, fuzz), module stock (spec-modules §7). Plan : docs/plan-overdrive.md.
//
//   entrée -> [passe-haut, passe-bas d'entrée] -> préaccentuation du modèle -> gain de drive (avec affaissement pour la lampe, modulé par la clé)
//          -> suréchantillonnage (Off, 2x, 4x) -> courbe de transfert du modèle -> retour à la cadence de base -> blocage du continu -> tonalité
//          -> désaccentuation du modèle -> [passe-haut, passe-bas de sortie] -> égaliseur 3 bandes (+ passe-haut, passe-bas) -> niveau de sortie
//
// Les modèles sont des courbes calculées, pas des copies de circuits. Le gain d'entrée / de sortie du ModuleRunner (stock::inputTrim, outputTrim) et le mélange
// sec / traité (stock::mix, aligné sur la latence du suréchantillonneur) sont communs ; le module rend la voie traitée seule.
// Modes de voies : stéréo, milieu / côtés, milieu seul, côtés seuls (la voie non traitée est retardée de la latence du suréchantillonneur).
// Entrée latérale : un suiveur d'enveloppe module le drive (source Off, entrée principale ou clé externe).
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "neptune/core/Biquad.h"
#include "neptune/core/Oversampler.h"
#include "neptune/core/StockModule.h"
#include "neptune/core/conv/eq.hpp"

namespace neptune::core
{
namespace overdrive
{
enum Model : int { kTube = 0, kTape, kTransistor, kFuzz, kNumModels };
enum Channels : int { kStereo = 0, kMidSide, kMidOnly, kSideOnly, kNumChannelModes };
enum KeySource : int { kKeyOff = 0, kKeyMain, kKeySide, kNumKeySources };

constexpr ParamId model = 100, drive = 101, bias = 102, character = 103, feel = 104, tone = 105, level = 106, autoGain = 107, oversampling = 108, channels = 109;
// filtres d'entrée et de sortie : actif, fréquence, pente (0 : 12 dB/oct, 1 : 24, 2 : 48)
constexpr ParamId inHpOn = 110, inHp = 111, inHpSlope = 112, inLpOn = 113, inLp = 114, inLpSlope = 115;
constexpr ParamId outHpOn = 116, outHp = 117, outHpSlope = 118, outLpOn = 119, outLp = 120, outLpSlope = 121;
// égaliseur : trois groupes de 5 (actif, type, fréquence, gain, Q), puis passe-haut et passe-bas
constexpr ParamId eqOn = 122;
constexpr ParamId bandBase = 123;
constexpr ParamId bandParam (int band, int k) { return bandBase + static_cast<ParamId> (5 * band + k); }   // 123..137
enum BandField : int { kOn = 0, kType = 1, kFreq = 2, kGain = 3, kQ = 4 };
constexpr ParamId eqHpOn = 138, eqHp = 139, eqHpSlope = 140, eqLpOn = 141, eqLp = 142, eqLpSlope = 143;
// entrée latérale : source, quantité (pourcentage de 30 dB de drive), attaque, retombée, passe-haut de la clé
constexpr ParamId keySource = 144, keyAmount = 145, keyAttack = 146, keyRelease = 147, keyHp = 148;
constexpr ParamId smooth = 149;   // douceur : 0 clair, 100 très adouci (fréquences des passe-bas de l'étage non linéaire)
constexpr ParamId tight = 150;          // graves nets : part des graves (sous ~180 Hz) retirée de la distorsion et ajoutée propre à la sortie
constexpr ParamId imperfection = 151;   // petites différences et dérive lente entre les deux voies (gain, polarisation), comme deux voies d'un vrai appareil
constexpr std::size_t numParams = 52;   // identifiants 100..151

enum Meter : int { kInPeakL = 0, kInPeakR, kOutPeakL, kOutPeakR, kKeyLevel, kNumMeters };   // pics linéaires depuis la dernière lecture ; kKeyLevel : 0..1

// Courbe de transfert statique d'un modèle : fonction impaire (sauf les modèles asymétriques) de pente voisine de 1 à l'origine, bornée près de +-1.
struct ShapeParams
{
    int model = kTube;
    double bias = 0.0;        // -1..+1
    double character = 0.5;   // 0..1 : dureté du coude
    double feel = 0.4;        // 0..1 : seuil de coupure du fuzz
    double smooth = 0.6;      // 0..1 : douceur (n'agit que dans HiStage, pas dans la courbe statique)
};
double shape (double x, const ShapeParams&) noexcept;

// Étage non linéaire à la cadence haute, avec la mémoire d'un vrai circuit (prototype « plus doux », 2026-10-31) : la courbe du modèle, un condensateur de liaison entre
// deux étages (il recentre la polarisation quand la courbe est asymétrique), un passe-bas entre les étages, puis un passe-bas à deux pôles après l'écrêteur
// (les aigus produits par l'écrêtage sont adoucis, comme par la tonalité d'une pédale). La courbe statique `shape()` en est la version sans filtres.
class HiStage
{
public:
    void configure (double fsHi, int model, double driveDb, double smooth01) noexcept;
    void reset() noexcept { couple_ = mid_ = post1_ = post2_ = 0.0; }
    double process (double x, const ShapeParams&) noexcept;

private:
    int model_ = kTube;
    double aCouple_ = 0.0, aMid_ = 0.0, aPost_ = 0.0;
    double couple_ = 0.0, mid_ = 0.0, post1_ = 0.0, post2_ = 0.0;
};
// Gain qui ramène le niveau efficace (alternatif) d'un sinus de -12 dBFS saturé vers celui d'entrée, pour un drive donné.
double autoGainFor (double driveLin, const ShapeParams&) noexcept;
const char* modelName (int m) noexcept;
} // namespace overdrive

class OverdriveModule : public StockModule
{
public:
    OverdriveModule();

    std::string_view typeId() const noexcept override { return "overdrive"; }
    bool declareParams (ParameterSet&, std::string* error) const override;
    void bind (const ParameterSet&) override;
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (float* left, float* right, int n, const ParameterSet& params) noexcept override;
    int latencySamples (const ParameterSet&) const noexcept override;
    void setZeroLatency (bool z) noexcept override { zero_.store (z, std::memory_order_relaxed); }
    int maxLatencySamples (double) const noexcept override { return Oversampler::kLatency4x; }

    bool wantsKey() const noexcept override { return true; }
    void setKey (const float* left, const float* right) noexcept override { keyL_ = left; keyR_ = right; }

    // Réponse en fréquence (dB) de l'égaliseur pour les réglages donnés (0 si l'égaliseur est coupé) : pour la courbe de l'interface, hors temps réel.
    static double eqResponseDb (const ParameterSet&, double freq, double sampleRate);
    static double eqBandResponseDb (const ParameterSet&, int band, double freq, double sampleRate);   // 0..2 : bandes, 3 : passe-haut, 4 : passe-bas

    int numMeters() const noexcept override { return overdrive::kNumMeters; }
    float meter (int i) const noexcept override
    {
        return i >= 0 && i < overdrive::kNumMeters ? meters_[static_cast<std::size_t> (i)].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    std::atomic<bool> zero_ { false };   // profil Live : variante à latence nulle (setZeroLatency)
    struct Chain
    {
        Oversampler os;
        ButterworthCascade inHp, inLp, outHp, outLp;
        Biquad pre, post1, post2, dc;   // préaccentuation, désaccentuation, bosse grave, blocage du continu
        overdrive::HiStage hi;
        double toneLp = 0.0, gain = 1.0, gbw = 0.0;   // gbw : état du passe-bas de bande passante du gain
        double sagFast = 0.0, sagSlow = 0.0, biasDyn = 0.0;   // affaissement de l'alimentation (deux constantes de temps) et décalage dynamique de la polarisation
        double lowLp = 0.0, lowLp2 = 0.0;                     // Tight : deux passe-bas en série qui isolent les graves
        double driftPhase1 = 0.0, driftPhase2 = 0.0;          // Imperfection : dérive lente
        double impDriveDb = 0.0, impBias = 0.0;               // Imperfection : valeurs courantes (recalculées tous les 16 échantillons)
        std::uint32_t driftTick = 0;
        double lastGainArg = 1.0e9, lastGainTarget = 1.0, gbwCoef = 1.0;     // dernier gain de drive calculé (évite une exponentielle par échantillon)
        std::vector<float> dynBuf;                            // polarisation dynamique, un échantillon par entrée
        int index = 0;                                        // 0 : première voie, 1 : seconde (signe de l'imperfection)
        std::vector<float> low;                               // graves du bloc, pour les rajouter propres après la distorsion
        std::array<float, 128> lowRing {};
        std::size_t lowPos = 0;
        std::array<float, 128> delay {};
        std::size_t delayPos = 0;
        void reset() noexcept;
    };

    double param (ParamId id) const noexcept { return v_[static_cast<std::size_t> (id - 100)]; }
    void readParams (const ParameterSet&) noexcept;
    void updateDesigns() noexcept;
    void processChunk (float* left, float* right, int len, const float* keyL, const float* keyR) noexcept;
    void processChain (Chain&, float* buf, int len) noexcept;
    void delayThrough (Chain&, float* buf, int len, int samples) noexcept;
    llc::EQParams makeEq() const noexcept;

    std::array<std::size_t, overdrive::numParams> idx_ {};
    std::array<double, overdrive::numParams> v_ {}, last_ {};
    bool haveLast_ = false;

    double fs_ = 48000.0;
    int maxBlock_ = 512;
    std::array<Chain, 2> chain_, old_;   // old_ : copie de chain_ avant un changement de suréchantillonnage, pendant le fondu enchaîné
    static constexpr int kFadeLen = 1024;
    int fadeLeft_ = 0;
    std::vector<float> oa_, ob_;
    llc::ParametricEQ eq_;
    Biquad keyHpFilter_;

    // état par bloc
    std::vector<float> a_, b_, keepA_, keepB_;
    std::vector<float> modDb_;      // modulation du drive par la clé, en dB, un échantillon par entrée
    overdrive::ShapeParams shape_ {}, shapeTarget_ {};
    double driveDb_ = 18.0, levelLin_ = 1.0, levelCur_ = 1.0, compTarget_ = 1.0, toneGainLow_ = 1.0, toneGainHigh_ = 1.0;
    double sagAttackF_ = 0.0, sagReleaseF_ = 0.0, sagAttackS_ = 0.0, sagReleaseS_ = 0.0, lowCoef_ = 0.0, lowBack_ = 1.0, tight_ = 0.0, imperfection_ = 0.0, driftInc1_ = 0.0, driftInc2_ = 0.0, keyEnv_ = 0.0, keyAttack_ = 0.0, keyRelease_ = 0.0, gainSmooth_ = 0.0, toneCoef_ = 0.0, levelSmooth_ = 0.0;
    int factor_ = 2, modelIndex_ = 0, channelMode_ = 0, keyMode_ = 0, latency_ = 0;
    bool smoothInit_ = false;

    const float *keyL_ = nullptr, *keyR_ = nullptr;
    std::array<std::atomic<float>, overdrive::kNumMeters> meters_ {};
    std::array<float, overdrive::kNumMeters> hold_ {};
};
} // namespace neptune::core
