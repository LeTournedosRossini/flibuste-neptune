#pragma once
// Diapason (nom provisoire) : générateur de signaux de test et petite boîte à rythme. Cœur portable (aucune dépendance à JUCE) ; l'adaptateur
// (adapter-juce/src/DiapasonPlugin.*) ne fait que relier l'hôte : paramètres, notes MIDI, position de lecture.
//
//  - Signal : formes d'onde, impulsion, deux sinus, continu, bruits colorés, balayages ; trois modes (continu, déclenché, note MIDI).
//  - Batterie : huit sons calculés, séquenceur d'une mesure à quatre temps (4 à 16 pas, vide / normal / accent, swing).
//  - Déclenchement : interface (postTrigger / postDrum), notes MIDI (noteOn / noteOff), paramètres d'hôte (marche, déclencheur, pas).
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "neptune/core/Biquad.h"
#include "neptune/core/Metronome.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/Tuner.h"

namespace neptune::core::generator
{
constexpr const char* moduleId = "diapason";

enum Signal : int { kSine = 0, kSquare, kTriangle, kSawUp, kSawDown, kImpulse, kTwoTone, kDc, kWhite, kPink, kBrown, kBlue, kViolet, kSweepLog, kSweepLin, kNumSignals };
enum Mode : int { kContinuous = 0, kTriggered, kMidi, kNumModes };
enum Polarity : int { kPolNormal = 0, kPolLeft, kPolRight, kPolBoth };
enum Sync : int { kSyncInternal = 0, kSyncHost };
enum Drum : int { kKick = 0, kSnare, kHatClosed, kHatOpen, kTom, kClap, kRim, kCowbell, kNumDrums };

constexpr int kMaxSteps = 16;
constexpr std::uint8_t kStepOff = 0, kStepNormal = 1, kStepAccent = 2;

// ---- identifiants (stables : ne changent jamais une fois publiés)
constexpr ParamId signal = 100, mode = 101, on = 102, freq = 103, level = 104, pan = 105, width = 106, freq2 = 107, level2 = 108;
constexpr ParamId sweepStart = 109, sweepEnd = 110, sweepTime = 111, length = 112, burstOn = 113, burstOff = 114, polarity = 115, trigger = 116;
constexpr ParamId drumNote = 117, master = 118;
constexpr ParamId seqRun = 130, seqSync = 131, tempo = 132, steps = 133, swing = 134, drumLevel = 135;

// Métronome (600..609) et accordeur (620..625)
constexpr ParamId metroOn = 600, metroSync = 601, metroTempo = 602, metroBeats = 603, metroUnit = 604, metroSub = 605, metroSound = 606, metroLevel = 607, metroAccent = 608, metroSubLevel = 609;
constexpr ParamId tunerA4 = 620, tunerPreset = 621, tunerTone = 622, tunerNote = 623, tunerCents = 624, tunerLevel = 625;

enum DrumKind : int { kDrumLevel = 0, kDrumTune, kDrumDecay, kDrumPan, kDrumMute };
constexpr ParamId drumParam (int drum, int kind) noexcept { return static_cast<ParamId> (200 + drum * 10 + kind); }
constexpr ParamId stepParam (int drum, int step) noexcept { return static_cast<ParamId> (400 + drum * 16 + step); }

const char* signalName (int s) noexcept;   // « Sine », « Pink noise »... (anglais, comme les noms de paramètres)
const char* drumName (int d) noexcept;

// Déclare tous les paramètres. Hors temps réel.
bool addParams (ParameterSet& set, std::string* error = nullptr);

// Position de lecture de l'hôte pour ce bloc (en temps de noire).
struct Transport
{
    bool valid = false;     // l'hôte donne une position
    bool playing = false;
    double ppq = 0.0;       // position au début du bloc, en noires
    double bpm = 120.0;
};

// État des bruits colorés (voir SignalGenerator.cpp).
struct ColourState
{
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, brown = 0, lastPink = 0, lastWhite = 0;
};

class GeneratorEngine
{
public:
    GeneratorEngine();

    ParameterSet& params() noexcept { return params_; }
    const ParameterSet& params() const noexcept { return params_; }
    bool ok() const noexcept { return ok_; }

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // --- thread de message : files sans verrou (compteurs) lues par le thread audio
    void postTrigger() noexcept { triggerCount_.fetch_add (1, std::memory_order_relaxed); }
    void postDrum (int drum) noexcept { if (drum >= 0 && drum < kNumDrums) drumCount_[static_cast<std::size_t> (drum)].fetch_add (1, std::memory_order_relaxed); }
    void postPanic() noexcept { panicCount_.fetch_add (1, std::memory_order_relaxed); }

    // --- thread audio
    void noteOn (int key, float velocity) noexcept;
    void noteOff (int key) noexcept;
    void allNotesOff() noexcept;
    // Écrase `left` et `right`.
    void process (float* left, float* right, int n, const Transport& transport) noexcept;

    // Entrée audio de l'instrument (accordeur) : à appeler avant `process`, qui écrase les tampons de sortie. Deuxième voie facultative (nullptr).
    void pushInput (const float* a, const float* b, int n) noexcept { tracker_.push (a, b, n); }

    // --- affichage (lisible depuis l'interface sans verrou)
    tuner::PitchTracker::Result tunerReading() const { return tracker_.analyse(); }   // thread de l'interface : calcule la hauteur de l'entrée
    int metronomeBeat() const noexcept { return metro_.beatInBar(); }
    std::uint32_t metronomeClicks() const noexcept { return metro_.clicks(); }
    float peakLeft() const noexcept { return peakL_.load (std::memory_order_relaxed); }
    float peakRight() const noexcept { return peakR_.load (std::memory_order_relaxed); }
    int currentStep() const noexcept { return step_.load (std::memory_order_relaxed); }   // -1 : séquenceur arrêté
    bool signalSounding() const noexcept { return sounding_.load (std::memory_order_relaxed); }
    double sweepPosition() const noexcept { return sweepPos_.load (std::memory_order_relaxed); }   // 0..1, balayage en cours

    // Fréquence (Hz) d'une note MIDI (La 69 = 440 Hz).
    static double noteFrequency (int key) noexcept;

private:
    struct Rng
    {
        std::uint32_t s = 2463534242u;
        float white() noexcept   // -1..1
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return static_cast<float> (static_cast<std::int32_t> (s)) * (1.0f / 2147483648.0f);
        }
    };
    struct DrumVoice
    {
        bool active = false;
        double t = 0.0;            // échantillons écoulés
        float velocity = 1.0f;
        double ph[6] {};           // phases (oscillateurs)
        Biquad f1, f2;             // filtres
        float tune = 1.0f;         // rapport de fréquence
        float decay = 1.0f;        // facteur de durée
        std::uint32_t seed = 1;
    };
    struct DrumSnapshot { float gain[kNumDrums]; float tune[kNumDrums]; float decay[kNumDrums]; float panL[kNumDrums]; float panR[kNumDrums]; bool mute[kNumDrums]; };

    void bindIndices();
    void startSignal (double freqHz, bool fromNote) noexcept;
    void triggerDrum (int drum, float velocity) noexcept;
    float renderDrum (int drum, DrumVoice&) noexcept;
    double p (std::size_t idx) const noexcept { return params_.effectiveAt (idx); }

    ParameterSet params_;
    bool ok_ = true;
    double sr_ = 48000.0;
    int maxBlock_ = 512;

    // index des paramètres
    std::size_t iSignal_ = 0, iMode_ = 0, iOn_ = 0, iFreq_ = 0, iLevel_ = 0, iPan_ = 0, iWidth_ = 0, iFreq2_ = 0, iLevel2_ = 0, iSweepStart_ = 0, iSweepEnd_ = 0, iSweepTime_ = 0;
    std::size_t iLength_ = 0, iBurstOn_ = 0, iBurstOff_ = 0, iPolarity_ = 0, iTrigger_ = 0, iDrumNote_ = 0, iMaster_ = 0;
    std::size_t iMetroOn_ = 0, iMetroSync_ = 0, iMetroTempo_ = 0, iMetroBeats_ = 0, iMetroUnit_ = 0, iMetroSub_ = 0, iMetroSound_ = 0, iMetroLevel_ = 0, iMetroAccent_ = 0, iMetroSubLevel_ = 0;
    std::size_t iTunerA4_ = 0, iTunerTone_ = 0, iTunerNote_ = 0, iTunerCents_ = 0, iTunerLevel_ = 0;
    tuner::PitchTracker tracker_;
    tuner::ToneGen tone_;
    metronome::Metronome metro_;
    std::vector<float> aux_;   // clic et note de référence d'un bloc, ajoutés à la sortie
    std::size_t iSeqRun_ = 0, iSeqSync_ = 0, iTempo_ = 0, iSteps_ = 0, iSwing_ = 0, iDrumLevel_ = 0;
    std::array<std::array<std::size_t, 5>, kNumDrums> iDrum_ {};
    std::array<std::array<std::size_t, kMaxSteps>, kNumDrums> iStep_ {};

    // signal
    Rng rng_;
    ColourState colour_;
    double phase_ = 0.0, phase2_ = 0.0;
    double tSig_ = 0.0;            // échantillons depuis le début du signal (balayage)
    double tBurst_ = 0.0;
    long long remain_ = -1;        // échantillons restants (déclenché / note), -1 : sans fin
    bool gate_ = false;            // le signal doit sonner
    bool lastOn_ = false, lastTrigger_ = false;
    float gain_ = 0.0f;            // fondu
    float ampLSm_ = 0.0f, ampRSm_ = 0.0f;   // gains de sortie lissés (niveau, panoramique, polarité)
    int lastMode_ = -1;
    bool smInit_ = false;
    double noteFreq_ = 440.0;
    bool noteMode_ = false;        // la fréquence vient de la note
    std::array<int, 16> held_ {};
    int heldCount_ = 0;
    std::uint32_t lastTriggerCount_ = 0, lastPanicCount_ = 0;
    std::array<std::uint32_t, kNumDrums> lastDrumCount_ {};
    std::atomic<std::uint32_t> triggerCount_ { 0 }, panicCount_ { 0 };
    std::array<std::atomic<std::uint32_t>, kNumDrums> drumCount_ {};

    // batterie
    std::array<DrumVoice, kNumDrums> voices_ {};

    // séquenceur
    bool seqPlaying_ = false;
    double seqPpq_ = 0.0;
    long long lastStepGlobal_ = std::numeric_limits<long long>::min();
    std::atomic<int> step_ { -1 };

    std::atomic<float> peakL_ { 0.0f }, peakR_ { 0.0f };
    std::atomic<bool> sounding_ { false };
    std::atomic<double> sweepPos_ { 0.0 };
};
} // namespace neptune::core::generator
