#pragma once
// « Digue » : limiteur brickwall à anticipation (lookahead), module stock (spec-modules §7). Plan : docs/plan-limiteur.md.
//
// Reconstruction par mesures de l'extérieur du limiteur de Reaper (spécification du banc : ~/Bureau/DAW/docs/realimit-specification-high-quality.md,
// modèle de référence tools/realimit/model_hq2.py) ; aucun code de Reaper n'est lu ni copié.
//
//   entrée -> gain de repos (plafond - seuil, dB) -> crête des échantillons, canaux liés -> gain nécessaire r = min (1, plafond / crête)
//          -> étage rapide (rampe avant la crête, droites vers les pics à venir, remontée de durée fixe)  \  gain appliqué = le plus bas
//          -> étage lent (en dB : monte à 4 x Release, descend à Release)                                  /  des deux, sur le signal retardé
//
// Pas d'écrêteur : sous le seuil, la sortie est le signal d'entrée multiplié par le gain de repos, sans autre changement.
// Deux modes : Quality (anticipation Lookahead, 10 ms = mode High Quality de ReaLimit) et Low Latency (anticipation de 1,18 ms, maintien du creux
// pendant 10,1 ms après le premier pic d'un groupe ; règle approchée, incomplète dans la mesure du banc). Toutes les durées sont en millisecondes :
// elles suivent la fréquence d'échantillonnage. La latence déclarée est la durée de l'anticipation.
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "neptune/core/StockModule.h"

namespace neptune::core
{
namespace limiter
{
constexpr ParamId threshold = 100;   // dB, -60..+12 : gain d'entrée relatif au plafond (gain de repos = Ceiling - Threshold)
constexpr ParamId ceiling = 101;     // dB, -24..0
constexpr ParamId release = 102;     // dB/s, 1..100
constexpr ParamId lookahead = 103;   // ms, 1..10 (mode Quality ; non automatisable : c'est la latence)
constexpr ParamId mode = 104;        // choix : 0 Quality, 1 Low Latency, 2 Zero Latency (non automatisable : c'est la latence)

enum Mode : int { kQuality = 0, kLowLatency = 1, kZeroLatency = 2, kNumModes };   // kZeroLatency : aucune anticipation, attaque instantanée (pour le direct)

constexpr double maxLookaheadMs = 10.0;
constexpr double lowLatencyMs = 1.18;        // latence déclarée en Low Latency (52 échantillons à 44,1 kHz, 56 à 48 kHz)
constexpr double lowLatencyHoldMs = 10.0;    // maintien du creux en Low Latency : 480 échantillons à 48 kHz, comptés depuis le pic (mesure du banc)
constexpr double riseMs = 10.0;              // remontée de l'étage rapide (480 échantillons à 48 kHz), durée fixe quelle que soit la profondeur
constexpr double rampShare = 0.2;            // rampe d'attaque au repos = 20 % de l'anticipation (96 échantillons pour 480)
constexpr double slowUp = 4.0;               // l'étage lent monte à 4 x Release

// Valeurs d'affichage (lues par l'interface sans verrou).
enum Meter : int { kInPeakL = 0, kInPeakR, kOutPeakL, kOutPeakR, kReduction, kNumMeters };   // crêtes linéaires ; réduction de gain en dB (positive)

// Anticipation en échantillons pour un mode et un réglage Lookahead (ms) donnés.
int lookaheadSamples (double sampleRate, int mode, double lookaheadMs) noexcept;
} // namespace limiter

class LimiterModule : public StockModule
{
public:
    LimiterModule();

    std::string_view typeId() const noexcept override { return "limiter"; }
    bool declareParams (ParameterSet&, std::string* error) const override;
    void bind (const ParameterSet&) override;
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (float* left, float* right, int n, const ParameterSet& params) noexcept override;
    int latencySamples (const ParameterSet&) const noexcept override;
    int maxLatencySamples (double sampleRate) const noexcept override;

    int numMeters() const noexcept override { return limiter::kNumMeters; }
    float meter (int i) const noexcept override
    {
        return i >= 0 && i < limiter::kNumMeters ? meters_[static_cast<std::size_t> (i)].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    void configure (int mode, int delay) noexcept;   // fixe l'anticipation, les durées et vide les tampons
    void step (float inL, float inR, float& outL, float& outR) noexcept;

    std::array<std::size_t, 5> idx_ {};
    double sampleRate_ = 48000.0;
    int maxDelay_ = 0;

    // configuration courante
    int mode_ = -1, delay_ = -1;
    int look_ = 0, rise_ = 1, hold_ = 0;
    double slewDefault_ = 0.0;

    // tampons circulaires : signal d'entrée amplifié (u) et gain nécessaire (r), indexés par l'instant absolu
    int cap_ = 0;
    std::vector<float> uL_, uR_, r_;
    std::int64_t n_ = 0;            // instant absolu du prochain échantillon d'entrée
    std::int64_t lastBelow_ = 0;    // dernier instant où r < 1
    // étage rapide
    double prev_ = 1.0, slope_ = 0.0;
    double redDb_ = 0.0;            // Zero Latency : réduction de gain courante (dB)
    std::int64_t holdUntil_ = 0;    // Low Latency : la remontée ne peut commencer qu'à cet instant (pic + maintien)
    // étage lent (dB)
    double slowDb_ = 0.0;
    double slowUp_ = 0.0, slowDown_ = 0.0;
    // gain de repos lissé (dB)
    bool boostSnap_ = true;
    double boostDb_ = 0.0, boostTargetDb_ = 0.0, boostLin_ = 1.0;
    double ceilLin_ = 1.0;

    std::array<std::atomic<float>, limiter::kNumMeters> meters_ {};
    float inPeak_[2] = { 0, 0 }, outPeak_[2] = { 0, 0 };
    double lastReductionDb_ = 0.0;
};
} // namespace neptune::core
