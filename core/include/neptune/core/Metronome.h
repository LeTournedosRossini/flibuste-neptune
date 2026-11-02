#pragma once
// Métronome de Diapason : clic sur chaque temps (et subdivision), temps fort accentué, tempo propre ou celui de l'hôte (alors calé sur la position de lecture).
// Cœur portable (aucune dépendance à JUCE). Les sons sont calculés (sinus amortis).
#include <atomic>
#include <cstdint>

namespace neptune::core::metronome
{
constexpr int kNumSounds = 4;     // clic, bois, bip, cloche

struct Settings
{
    bool run = false;
    bool hostSync = false;         // suit la lecture et le tempo de l'hôte (muet à l'arrêt)
    double bpm = 120.0;            // tempo propre (par noire)
    int beats = 4;                 // temps par mesure (1..12)
    int unit = 4;                  // valeur du temps : 4 (noire), 8 (croche), 16 (double-croche)
    int sub = 1;                   // clics par temps (1..4)
    int sound = 0;
    double levelDb = -12.0;
    double accentDb = 6.0;         // renfort du premier temps
    double subDb = -6.0;           // niveau des subdivisions par rapport au temps
};

struct HostPos
{
    bool valid = false, playing = false;
    double ppq = 0.0, bpm = 120.0;   // au début du bloc, en noires
};

class Metronome
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    // Écrase out[0..n) avec le clic (mono).
    void render (const Settings&, const HostPos&, float* out, int n) noexcept;

    // Affichage : numéro du temps joué dans la mesure (0 = premier), -1 à l'arrêt ; `clicks` augmente à chaque temps (pour détecter un nouveau temps).
    int beatInBar() const noexcept { return beat_.load (std::memory_order_relaxed); }
    std::uint32_t clicks() const noexcept { return clicks_.load (std::memory_order_relaxed); }

private:
    struct Voice
    {
        bool active = false;
        double phase1 = 0.0, phase2 = 0.0, f1 = 0.0, f2 = 0.0;
        double age = 0.0, decay = 0.01, amp = 0.0, mix2 = 0.0;
    };
    void trigger (const Settings&, bool accent, bool sub) noexcept;

    double sr_ = 48000.0;
    Voice voices_[3] {};
    int next_ = 0;
    long long lastTick_ = 0;
    bool haveTick_ = false;
    double internalTick_ = 0.0;
    bool wasActive_ = false;
    std::atomic<int> beat_ { -1 };
    std::atomic<std::uint32_t> clicks_ { 0 };
};
} // namespace neptune::core::metronome
