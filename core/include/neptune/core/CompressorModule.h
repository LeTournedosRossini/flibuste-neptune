#pragma once
// Compresseur (série NOVA), PORTÉ de ZL Compressor de zsliu98 (https://github.com/ZL-Audio/ZLCompressor, AGPLv3 comme ce projet) : quatre styles
// (Clean, Classic, Optical, Vocal), quatre directions (compression, gonflement, expansion, « shape »), courbe seuil / ratio / genou / courbe /
// plancher, suiveur attaque / relâchement / pump-punch / smooth, plage, maintien, RMS en parallèle, liaison et modes stéréo (M/S, L/R, avec max),
// sidechain externe avec égaliseur de détection (8 filtres) et écoute, écrêteur tanh, lookahead (négatif : retard de la clé), suréchantillonnage,
// delta. Voir core/DynamicsDsp.h pour les briques et leurs crédits ; le détail des écarts avec ZL est dans docs/passage-de-relais.md.
// Copyright (C) 2025-2026 zsliu98 pour les algorithmes d'origine ; adaptation pour ce projet.
#include <array>
#include <atomic>
#include <vector>

#include "neptune/core/AnalyzerRing.h"
#include "neptune/core/Biquad.h"
#include "neptune/core/DynamicsDsp.h"
#include "neptune/core/MidiKey.h"
#include "neptune/core/Oversampler.h"
#include "neptune/core/StockModule.h"

namespace neptune::core
{
namespace compressor
{
constexpr ParamId style = 100, direction = 101, threshold = 102, ratio = 103, kneeWidth = 104, curve = 105, floorDb = 106, attack = 107, release = 108,
                  pump = 109, smooth = 110, range = 111, rangeInf = 112, hold = 113, makeup = 114, wet = 115;
constexpr ParamId externalSide = 116, sideOut = 117, sideEqBypass = 118, sideGain = 119;
constexpr ParamId stereoMode = 120, stereoSwap = 121, stereoLink = 122, wet1 = 123, wet2 = 124;
constexpr ParamId delta = 125, clipperDrive = 126, oversample = 127, lookahead = 128;
constexpr ParamId rmsOn = 129, rmsLength = 130, rmsSpeed = 131, rmsMix = 132;
// Source MIDI du sidechain (`externalSide` vaut 2) : forme de la correspondance vélocité -> niveau de la clé (-100..+100 %) et canal MIDI (0 = tous, 1..16 ; réglage non automatisable).
constexpr ParamId midiShape = 198, midiChannel = 199;
enum KeySource : int { kSourceInternal = 0, kSourceExternal, kSourceMidi };
// Réglages d'affichage de l'analyseur (cachés, non automatisables, enregistrés avec l'état ; hors presets et A/B) : durée affichée (6, 9, 12, 18 s), défilement
// (Sync, Slow, Roll), plage haute (6, 0, -6, -12, -24 dB) et basse (-9 ... -120 dB), crête ou RMS, canaux, courbes visibles.
constexpr ParamId anTime = 188, anMove = 189, anMaxDb = 190, anMinDb = 191, anMag = 192, anStereo = 193, anShowIn = 194, anShowOut = 195, anShowRed = 196, anShowKey = 197;
// Flux de l'anneau d'analyse (comme ZL) : entrée alignée sur la latence, sortie compressée (avant le gain de compensation), sortie finale, clé.
enum AnalyzerStream : int { kAnPre = 0, kAnCompressed, kAnPost, kAnKey, kAnStreams };
constexpr int kBands = 8;
// Filtres de la clé : base 140, six paramètres par bande (état, type, pente, fréquence, gain, Q).
constexpr ParamId sideBandBase = 140;
constexpr ParamId bandParam (int band, int k) { return sideBandBase + static_cast<ParamId> (band * 6 + k); }
enum BandParam : int { kStatus = 0, kType, kOrder, kFreq, kGain, kQ };
enum FilterStatus : int { kOff = 0, kBypassed, kOn };
enum FilterType : int { kPeak = 0, kLowShelf, kLowPass, kHighShelf, kHighPass, kNotch, kBandPass, kTiltShelf };
constexpr double maxLookaheadMs = 20.0;

// Valeurs d'affichage.
enum Meter : int { kInPeakL = 0, kInPeakR, kOutPeakL, kOutPeakR, kReductionL, kReductionR, kNumMeters };   // réduction en dB (<= 0 : réduction, > 0 : gain)
} // namespace compressor

class CompressorModule : public StockModule
{
public:
    CompressorModule();

    std::string_view typeId() const noexcept override { return "compressor"; }
    bool declareParams (ParameterSet&, std::string* error) const override;
    void bind (const ParameterSet&) override;
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (float* left, float* right, int n, const ParameterSet& params) noexcept override;
    int latencySamples (const ParameterSet&) const noexcept override;
    void setZeroLatency (bool z) noexcept override { zero_.store (z, std::memory_order_relaxed); }
    int maxLatencySamples (double sampleRate) const noexcept override;
    bool wantsKey() const noexcept override { return true; }
    void setKey (const float* left, const float* right) noexcept override { keyL_ = left; keyR_ = right; }

    int numMeters() const noexcept override { return compressor::kNumMeters; }
    float meter (int i) const noexcept override
    {
        return i >= 0 && i < compressor::kNumMeters ? meters_[static_cast<std::size_t> (i)].load (std::memory_order_relaxed) : 0.0f;
    }
    // Courbe de gain courante (pour l'affichage) : niveau de sortie (dB) pour un niveau d'entrée (dB).
    double curveOutputDb (const ParameterSet&, double inputDb) const noexcept;
    // Source MIDI : l'adaptateur y pousse les messages MIDI de chaque bloc (voir MidiKey) ; sans effet tant que la source n'est pas « MIDI ».
    MidiKey& midiKey() noexcept { return midiKey_; }
    const MidiKey& midiKey() const noexcept { return midiKey_; }
    // Anneau d'analyse pour l'historique ; l'interface l'active tant qu'un éditeur est ouvert (le remplissage est sinon sauté).
    AnalyzerRing& analyzer() const noexcept { return analyzer_; }
    // Réponse (dB) d'un filtre de la clé à la fréquence f (0 dB si le filtre n'est pas en service), ou de l'ensemble des filtres en service.
    // Mêmes coefficients que le DSP ; `fs` est la fréquence d'échantillonnage de travail (sans suréchantillonnage).
    static double sideBandResponseDb (const ParameterSet&, int band, double freq, double fs) noexcept;
    static double sideResponseDb (const ParameterSet&, double freq, double fs) noexcept;

private:
    std::atomic<bool> zero_ { false };   // profil Live : variante à latence nulle (setZeroLatency)
    struct Settings
    {
        dyn::Style style = dyn::Style::Clean;
        dyn::Direction direction = dyn::Direction::Compress;
        double threshold = -32.0, ratio = 3.0, knee = 8.0, curve = 0.0, floorDb = -140.0, attack = 100.0, release = 500.0, pump = 0.0, smooth = 0.0;
        double range = 80.0, holdMs = 0.0, makeup = 0.0, wet = 1.0, sideGainDb = 0.0, link = 0.0, wet1 = 1.0, wet2 = 1.0, clipper = 0.0;
        double rmsLengthMs = 32.0, rmsSpeed = 1.0, rmsMix = 0.5, lookaheadMs = 0.0, midiShape = 0.0;
        bool rangeInf = false, ext = false, midi = false, sideOut = false, eqBypass = false, midSide = true, stereoMax = false, swap = false, deltaOn = false, rmsOn = false;
        int oversample = 0, midiChannel = 0;
    };
    struct SideBand
    {
        int status = 0, type = 0, order = 1;
        double freq = 1000.0, gain = 0.0, q = 0.707;
        bool same (const SideBand& o) const noexcept { return status == o.status && type == o.type && order == o.order && freq == o.freq && gain == o.gain && q == o.q; }
    };
    struct SideFilter
    {
        ButterworthCascade cascade;                     // passe-haut / passe-bas
        std::array<Biquad, 6> sections;                 // autres types (cascade de `count` sections)
        int count = 0;
        bool butterworth = false;
        void reset() noexcept { cascade.reset(); for (auto& s : sections) s.reset(); }
        float process (float x) noexcept
        {
            if (butterworth) return cascade.process (x);
            for (int i = 0; i < count; ++i) x = sections[static_cast<std::size_t> (i)].process (x);
            return x;
        }
    };

    void readSettings (const ParameterSet&) noexcept;
    void designSideBand (int band, int channel) noexcept;
    void applySettings() noexcept;
    void processHighRate (float* mainL, float* mainR, float* sideL, float* sideR, int n) noexcept;
    void styleProcess (int channel, float* buf, int n) noexcept;
    void rmsProcess (int channel, const float* in, float* out, int n) noexcept;
    double param (ParamId id) const noexcept { return values_[static_cast<std::size_t> (id - 100)]; }

    std::array<std::size_t, 100> idx_ {};   // ids 100..199
    std::array<double, 100> values_ {};
    double sampleRate_ = 48000.0;
    int maxBlock_ = 0, factor_ = 1;

    Settings settings_;
    dyn::Style appliedStyle_ = dyn::Style::Clean;
    dyn::Direction appliedDirection_ = dyn::Direction::Compress;
    double appliedRate_ = 0.0;
    bool haveApplied_ = false;
    std::array<SideBand, compressor::kBands> bands_ {}, appliedBands_ {};
    std::array<std::array<SideFilter, compressor::kBands>, 2> sideFilters_;
    bool bandsDirty_ = true;

    dyn::GainComputer computer_;
    std::array<dyn::Follower, 2> follower_, rmsFollower_;
    std::array<dyn::RmsTracker, 2> tracker_;
    std::array<double, 2> feedback_ { 0.0, 0.0 };   // Classic et Vocal : dernier échantillon sorti
    std::array<dyn::MinHold, 2> hold_;
    dyn::TanhClipper clipper_;
    std::array<Oversampler, 4> os_;                 // principal L, R, clé L, R
    std::array<dyn::IntegerDelay, 2> lookMain_, lookSide_;
    std::array<dyn::IntegerDelay, 2> preDelay_;     // signal d'entrée aligné sur la latence (delta)
    std::array<dyn::IntegerDelay, 2> keyDelay_;     // clé alignée de la même façon (analyseur)
    std::vector<float> keyAnL_, keyAnR_;
    mutable AnalyzerRing analyzer_ { compressor::kAnStreams, 2 };
    double makeupGain_ = 1.0, makeupTarget_ = 1.0;

    std::vector<float> sideL_, sideR_, preL_, preR_, postL_, postR_, rmsL_, rmsR_, sideEqL_, sideEqR_;
    std::vector<float> midiBuf_;
    MidiKey midiKey_;
    const float *keyL_ = nullptr, *keyR_ = nullptr;
    std::array<std::atomic<float>, compressor::kNumMeters> meters_ {};
};
} // namespace neptune::core
