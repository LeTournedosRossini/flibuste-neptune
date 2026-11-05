#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "NovaKit.h"
#include "neptune/core/Arpeggiator.h"
#include "neptune/core/ChordMapEnvironment.h"
#include "neptune/core/ChordPattern.h"
#include "neptune/core/ChordTimeline.h"
#include "neptune/core/SharedChordMap.h"
#include "neptune/core/Undo.h"
#include "neptune/core/Settings.h"
#include "neptune/core/Theme.h"

// « Arpège » (nom provisoire) : arpégiateur. Reçoit des notes tenues, sort des notes calées sur la grille de l'hôte (instant exact dans le bloc) vers
// l'instrument suivant. Cœur : neptune::core::arp::Arpeggiator. Voir docs/plan-harmonie-arpegiateur.md (phase 2 ; la lecture de la carte d'accords = phase 3).
// Les messages MIDI qui ne sont pas des notes (contrôleurs, roue de hauteur...) passent sans changement.
class ArpegeProcessor : public juce::AudioProcessor
{
public:
    ArpegeProcessor();
    ~ArpegeProcessor() override;

    void prepareToPlay (double sampleRate, int) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Arpège"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioParameterChoice *pattern = nullptr, *rate = nullptr, *velMode = nullptr;
    juce::AudioParameterInt *octaves = nullptr, *velocity = nullptr, *accentEvery = nullptr, *accentBoost = nullptr, *transpose = nullptr;
    juce::AudioParameterFloat *gate = nullptr, *swing = nullptr;
    juce::AudioParameterBool* hold = nullptr;
    juce::AudioParameterInt* slot = nullptr;           // 0 : motifs automatiques ; 1..16 : motif dessiné dans la matrice
    juce::AudioParameterChoice* absentMode = nullptr;  // degré absent de l'accord : sauter / jouer la note par défaut / prendre la note de la gamme (Carte)
    juce::AudioParameterChoice* source = nullptr;      // 0 : notes tenues et mesures de l'hôte ; 1 : Carte (accord, gamme, motif, mesures)
    juce::AudioParameterInt* carteChannel = nullptr;   // canal de liaison de Carte, 1 à 8
    juce::AudioParameterChoice* approachMode = nullptr; // accord d'approche avant un changement d'accord : non / dominante / triton / chromatique / parallèle
    juce::AudioParameterChoice* strumMode = nullptr;   // grattage des parties « accord » : non / coup bas / alterné
    juce::AudioParameterInt* strumDelay = nullptr;     // délai entre deux notes (ms), 0 = automatique d'après le tempo
    juce::AudioParameterBool* ledVoices = nullptr;     // voix conduites : les motifs en degrés se posent au plus près du coup précédent (sinon seulement ceux qui le demandent)
    juce::AudioParameterInt* baseOctave = nullptr;     // octave où Carte joue seule (sans note tenue) : 1 à 6 (3 = do 48)

    static constexpr int kSlots = 16;
    // Emplacements de motifs (fil de messages). `slotPattern` : copie du motif (vide : emplacement libre).
    bool slotUsed (int index) const;
    neptune::core::pattern::Pattern slotPattern (int index) const;
    void setSlotPattern (int index, neptune::core::pattern::Pattern p);
    void clearSlot (int index);
    // Changement annulable d'un motif (matrice, mode, longueur, chargement...) : `before` -> `after`.
    void commitSlot (int index, const neptune::core::pattern::Pattern& before, const neptune::core::pattern::Pattern& after, const char* name);
    neptune::core::UndoManager& undo() { return undo_; }
    static std::vector<neptune::core::pattern::Pattern> builtinPatterns();

    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    int heldCount() const noexcept { return held_.load(); }
    // Ce que Carte dit : -1 pas connectée (ou source « notes tenues »), 0 connectée sans accord ici, sinon 1 ; `chordText` : l'accord en cours.
    int carteState() const noexcept { return carteState_.load(); }
    bool carteHasPatterns() const noexcept { return carteHasPatterns_.load(); }   // la voie des motifs de Carte a des éléments (et Carte est la source)
    juce::String carteChordText() const;
    bool hostPlaying() const noexcept { return playing_.load(); }

    static const std::array<double, 8>& rateBeats();

private:
    neptune::core::arp::Params currentParams() const;

    neptune::core::arp::Arpeggiator arp_;
    neptune::core::settings::GlobalSettings settings_;
    std::vector<neptune::core::arp::NoteIn> in_;
    std::vector<neptune::core::arp::NoteOut> out_;
    juce::MidiBuffer scratch_;
    std::atomic<int> held_ { 0 };

    neptune::core::UndoManager undo_;
    mutable juce::SpinLock slotLock_;
    std::array<std::shared_ptr<const neptune::core::pattern::Pattern>, kSlots> slots_;
    std::vector<std::shared_ptr<const neptune::core::pattern::Pattern>> retired_;   // gardés en vie : le fil audio peut encore les lire
    std::atomic<int> slotsVersion_ { 0 };
    neptune::core::arp::ChordMapEnvironment::Slots audioSlots_;   // fil audio : copie des emplacements
    const neptune::core::pattern::Pattern* activePattern_ = nullptr;
    int activeSlot_ = -1, activeVersion_ = -1;

    // Carte : lecteur de la carte partagée (relevé et connecté depuis le fil de messages, consulté sans verrou par le fil audio)
    struct Ticker : juce::Timer
    {
        explicit Ticker (ArpegeProcessor& p) : owner (p) {}
        void timerCallback() override { owner.tick(); }
        ArpegeProcessor& owner;
    };
    void tick();
    neptune::core::chordmap::Reader reader_;
    std::unique_ptr<neptune::core::arp::ChordMapEnvironment> env_;
    Ticker ticker_ { *this };
    std::atomic<int> carteState_ { -1 };
    std::atomic<bool> carteHasPatterns_ { false };
    std::atomic<std::uint32_t> carteChord_ { 0 };   // racine | basse << 8 | rel << 16 ; 0 : aucun accord
    std::atomic<bool> playing_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArpegeProcessor)
};

class ArpegeEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ArpegeEditor (ArpegeProcessor&);
    ~ArpegeEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ParamPill;
    class MatrixView;
    bool keyPressed (const juce::KeyPress&) override;
    void editSlot (const char* name, const std::function<void (neptune::core::pattern::Pattern&)>& edit);
    void timerCallback() override;
    void refreshActions();

    ArpegeProcessor& proc_;
    nova::Skin skin_;
    juce::ComponentBoundsConstrainer constrainer_;
    std::vector<std::unique_ptr<ParamPill>> pills_;
    std::unique_ptr<MatrixView> matrix_;
    std::vector<std::unique_ptr<nova::Pill>> actions_;
    int fournis_ = 0, shownSlot_ = -2;
    std::unique_ptr<juce::FileChooser> chooser_;
};
