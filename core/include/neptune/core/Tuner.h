#pragma once
// Accordeur de Diapason : notes et accordages, correspondance d'une fréquence avec la note ou la corde la plus proche, détection de hauteur en continu (YIN) sur
// l'entrée audio, et note de référence. Cœur portable (aucune dépendance à JUCE).
//  - Le thread audio ne fait que déposer l'entrée (mono, sous-échantillonnée) dans un anneau sans verrou (PitchTracker::push).
//  - Le thread de l'interface lit l'anneau et calcule la hauteur (PitchTracker::analyse) : la charge n'est pas dans le chemin audio.
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace neptune::core::tuner
{
constexpr double kDefaultA4 = 440.0;

double noteFrequency (double midi, double a4 = kDefaultA4) noexcept;       // `midi` peut être fractionnaire (La 69 = a4)
double frequencyToMidi (double hz, double a4 = kDefaultA4) noexcept;       // 0 si la fréquence est invalide
std::string noteName (int midi, bool french);                                // « E2 », « Mi2 » (dièses, octave scientifique : 60 = C4 / Do4)

// Un accordage : les notes MIDI de ses cordes, de la première à la dernière dans l'ordre où on les nomme (graves vers aiguës ; l'ukulélé garde son sol aigu en premier).
// Sans corde : accordeur chromatique.
struct Tuning
{
    const char* en;
    const char* fr;
    std::vector<int> notes;
};
const std::vector<Tuning>& tunings();            // le premier est le chromatique
int numTunings() noexcept;

struct Match
{
    bool valid = false;
    int stringIndex = -1;       // corde visée (-1 : chromatique)
    int midi = 0;               // note visée
    double cents = 0.0;         // écart de la fréquence mesurée à la note visée (> 0 : trop haut)
};
// La note ou la corde la plus proche de `hz` pour l'accordage `tuningIndex` (hors plage : chromatique).
Match matchFrequency (double hz, double a4, int tuningIndex) noexcept;

// Détection de hauteur monophonique sur l'entrée (méthode YIN : différence normalisée, seuil, interpolation parabolique).
class PitchTracker
{
public:
    struct Result
    {
        bool valid = false;     // une hauteur claire a été trouvée
        double freq = 0.0;      // Hz
        float clarity = 0.0f;   // 0..1
        float levelDb = -120.0f;   // niveau efficace de la fenêtre analysée
    };
    static constexpr double kMinHz = 24.0, kMaxHz = 1500.0;

    void prepare (double sampleRate) noexcept;
    // Thread audio : une ou deux voies (la deuxième peut être nulle) ; fait la moyenne et sous-échantillonne vers environ 24 kHz.
    void push (const float* a, const float* b, int n) noexcept;
    // Thread de l'interface : analyse les dernières dizaines de millisecondes.
    Result analyse() const;
    double decimatedRate() const noexcept { return rate_; }

private:
    static constexpr int kRing = 4096;
    std::array<std::atomic<float>, kRing> ring_ {};
    std::atomic<std::uint32_t> write_ { 0 };
    double sr_ = 48000.0, rate_ = 24000.0;
    int factor_ = 4, count_ = 0;
    float acc_ = 0.0f;
};

// Note de référence : sinus entretenu, avec un fondu à l'entrée et à la sortie.
class ToneGen
{
public:
    void prepare (double sampleRate) noexcept { sr_ = sampleRate > 0.0 ? sampleRate : 48000.0; }
    void reset() noexcept { phase_ = 0.0; gain_ = 0.0f; }
    // Ajoute le sinus à `out` (n échantillons).
    void render (float* out, int n, bool on, double hz, float amplitude) noexcept;

private:
    double sr_ = 48000.0, phase_ = 0.0;
    float gain_ = 0.0f;
};
} // namespace neptune::core::tuner
