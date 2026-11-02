#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "HostProbe.h"
#include "ParamHost.h"
#include "NovaDiapasonEditor.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Settings.h"
#include "neptune/core/SignalGenerator.h"
#include "neptune/core/Theme.h"
#include "neptune/core/Undo.h"

// « Diapason » (nom provisoire) : générateur de signaux de test et petite boîte à rythme, publié comme plugin CLAP instrument. Le cœur (SignalGenerator.h) fait tout le
// travail ; ici : les paramètres de l'hôte, les notes MIDI (découpe du bloc à chaque événement), la position de lecture et l'état.
class DiapasonProcessor : public juce::AudioProcessor,
                          public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                          public ParamHost
{
public:
    DiapasonProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Diapason"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }   // messages MIDI envoyés à la main vers l'hôte (page MIDI) ; le tampon MIDI est vidé après usage : aucun écho
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ParamHost
    neptune::core::ParameterSet& parameters() override { return engine_.params(); }
    void uiBeginEdit (neptune::core::ParamId) override;
    void uiSetNormalised (neptune::core::ParamId, double) override;
    void uiEndEdit (neptune::core::ParamId) override;
    void uiResetToDefault (neptune::core::ParamId) override;
    bool uiUndo() override;
    bool uiRedo() override;
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0]; }
    void setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s) override { modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0] = s; }

    // Message MIDI envoyé à la main vers la sortie MIDI de l'hôte (page MIDI). Thread de message ; faux si la file est pleine ou le message vide.
    bool postMidi (const std::vector<std::uint8_t>& bytes);
    bool supportsOutboundEvents() override { return true; }
    void addOutboundEventsToQueue (const clap_output_events* out, const juce::MidiBuffer& midi, int sampleOffset) override;

    neptune::core::generator::GeneratorEngine& engine() { return engine_; }
    neptune::core::UndoManager& undoManager() { return undo_; }
    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    void applySettings (const neptune::core::settings::GlobalSettings&);   // met à jour et enregistre les réglages globaux de la suite (thème, contraste, langue)

private:
    static BusesProperties makeBuses();

    neptune::core::generator::GeneratorEngine engine_;
    HostProbe probe_;
    neptune::core::UndoManager undo_;
    neptune::core::settings::GlobalSettings settings_;
    neptune::core::uistate::Size modal_[2] {};
    struct MidiPacket
    {
        std::uint16_t size = 0;
        std::array<std::uint8_t, 1024> bytes {};
    };
    std::array<MidiPacket, 32> midiPackets_ {};   // file du thread de message vers le thread audio
    juce::AbstractFifo midiFifo_ { 32 };
    std::array<MidiPacket, 32> hostMidi_ {};      // thread audio : à envoyer à l'hôte à la fin du bloc
    int numHostMidi_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiapasonProcessor)
};

class DiapasonPluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit DiapasonPluginEditor (DiapasonProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    DiapasonProcessor& processor_;
    neptune::core::i18n::Translator tr_;
    void buildContent();   // (re)crée l'interface : au démarrage et quand le thème, le contraste ou la langue changent
    std::unique_ptr<NovaDiapasonEditor> content_;
    juce::ComponentBoundsConstrainer constrainer_;
};
