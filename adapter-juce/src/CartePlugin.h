#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>
#include "NovaKit.h"
#include "neptune/core/CarteScore.h"
#include "neptune/core/ChordTimeline.h"
#include "neptune/core/Settings.h"
#include "neptune/core/SharedChordMap.h"
#include "neptune/core/Theme.h"
#include "neptune/core/Undo.h"

// « Carte » (nom provisoire) : timeline d'accords, de gammes, de tonalités et de motifs d'Arpège que l'utilisateur place sur une piste, avec les mesures réelles
// du morceau (importées de Scorsaire). Tout est publié dans la carte partagée (SharedChordMap.h, format v2) pour les autres plugins de la suite ; la carte est
// enregistrée dans l'état du plugin (donc dans le projet). Le son passe sans changement. Voir docs/plan-harmonie-arpegiateur.md.
class CarteProcessor : public juce::AudioProcessor
{
public:
    CarteProcessor();
    ~CarteProcessor() override;

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Carte"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- la carte (thread de message)
    neptune::core::carte::Score score() const;
    // Remplace la carte ; `publish` faux pendant un glissement (une seule publication à la fin).
    void setScore (neptune::core::carte::Score, bool publish = true);
    // Changement annulable : `before` -> `after`.
    void commit (const neptune::core::carte::Score& before, const neptune::core::carte::Score& after, const char* name);
    neptune::core::UndoManager& undo() { return undo_; }

    int channel() const noexcept { return channel_.load(); }
    void setChannel (int c);
    neptune::core::chords::Naming naming() const noexcept { return naming_.load() == 1 ? neptune::core::chords::Naming::French : neptune::core::chords::Naming::Letters; }
    void setNaming (neptune::core::chords::Naming n) { naming_.store (n == neptune::core::chords::Naming::French ? 1 : 0); }
    // -1 pas encore ouvert, sinon neptune::core::chordmap::Status
    int writerStatus() const noexcept { return writerStatus_.load(); }

    // Position de l'hôte au dernier bloc (pour la tête de lecture)
    struct Transport
    {
        bool hasPosition = false, playing = false;
        double beats = 0.0, beatsPerBar = 4.0;
    };
    Transport transport() const noexcept;

    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }

private:
    struct Ticker : juce::Timer
    {
        explicit Ticker (CarteProcessor& p) : owner (p) {}
        void timerCallback() override { owner.tick(); }
        CarteProcessor& owner;
    };
    void tick();
    void publishNow();

    neptune::core::chordmap::Writer writer_;
    mutable juce::CriticalSection lock_;
    neptune::core::carte::Score score_;
    neptune::core::UndoManager undo_;
    neptune::core::settings::GlobalSettings settings_;
    std::atomic<int> channel_ { 1 }, naming_ { 0 }, writerStatus_ { -1 };
    int openedChannel_ = 0;
    std::atomic<double> beats_ { 0.0 }, beatsPerBar_ { 4.0 };
    std::atomic<bool> hasPosition_ { false }, playing_ { false };
    Ticker ticker_ { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CarteProcessor)
};

class CarteEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit CarteEditor (CarteProcessor&);
    ~CarteEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class TimelineView;
    class Popup;
    void timerCallback() override;
    void layoutAndScale();
    void refreshPills();
    // fenêtres de choix (au-dessus de la timeline)
    void showChordBuilder (double start, double length, int index);
    void showScalePicker (double start, double length, int index);
    void showKeyPicker (double start, int index);
    void showPatternPicker (double start, double length, int index);
    void showGlobalKeyPicker();
    void importFile();
    void closePopup();

    CarteProcessor& proc_;
    nova::Skin skin_;
    juce::ComponentBoundsConstrainer constrainer_;
    nova::Pill channelPill_, gridPill_, namingPill_, zoomOut_, zoomIn_, followPill_, undoPill_, redoPill_;
    nova::Pill copyPill_, pastePill_, dupPill_, importPill_, keyPill_, clearBarsPill_;
    std::unique_ptr<TimelineView> view_;
    std::unique_ptr<Popup> popup_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String hint_;
    std::uint64_t lastUndoRevision_ = ~0ull;
};
