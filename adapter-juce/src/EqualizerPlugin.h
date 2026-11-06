#pragma once
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <memory>
#include "HostProbe.h"
#include "ParamHost.h"
#include "ReserveHost.h"
#include "NovaEqualizerEditor.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Settings.h"
#include "neptune/core/StockModule.h"
#include "neptune/core/Theme.h"
#include "neptune/core/Undo.h"

// « Egaliseur » (nom provisoire) : l'égaliseur de la suite (porté de ZL Equalizer), publié comme plugin CLAP indépendant. Même DSP et même interface
// que l'insert de Neptune (EqualizerModule / NovaEqualizerEditor), autre adaptateur : ici, c'est l'hôte qui porte le plugin. Entrée sidechain de l'hôte.
class EqualizerProcessor : public juce::AudioProcessor, private juce::Timer,
                             public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                             public ParamHost
{
public:
    EqualizerProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Egaliseur"; }
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

    // ParamHost
    neptune::core::ParameterSet& parameters() override { return runner_.params(); }
    void uiBeginEdit (neptune::core::ParamId) override;
    void uiSetNormalised (neptune::core::ParamId, double) override;
    void uiEndEdit (neptune::core::ParamId) override;
    void uiResetToDefault (neptune::core::ParamId) override;
    bool uiUndo() override;
    bool uiRedo() override;
    // Réserve d'automation : l'hôte ne voit que 128 emplacements ; un réglage s'y relie par clic droit (ReserveMenu.h) ou depuis la fenêtre AUTOMATION.
    int reserveCount() const override { return reserve_.count(); }
    bool reserveCanAssign (neptune::core::ParamId) const override;
    int reserveSlotOf (neptune::core::ParamId id) const override { return reserve_.slotOf (id); }
    std::string reserveSlotName (int slot) const override { return reserve_.slotName (slot); }
    bool reserveLocked (int slot) const override { return reserve_.locked (slot); }
    int uiReserveAssign (neptune::core::ParamId id, int slot, bool replace) override { return reserve_.assign (*this, id, slot, replace); }
    void uiReserveRelease (neptune::core::ParamId id) override { if (const int k = reserve_.slotOf (id); k >= 0) reserve_.release (*this, k); }
    void uiReserveReleaseSlot (int slot) override { reserve_.release (*this, slot); }
    void uiReserveSetLocked (int slot, bool locked) override { reserve_.setLocked (*this, slot, locked); }
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0]; }
    void setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s) override { modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0] = s; }

    neptune::core::ModuleRunner& runner() { return runner_; }
    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    void applySettings (const neptune::core::settings::GlobalSettings&);   // met à jour et enregistre les réglages globaux de la suite (thème, contraste, langue)

private:
    static BusesProperties makeBuses();
    void timerCallback() override;   // thread de message : la latence déclarée suit lookahead et oversampling
    int declaredLatency_ = -1;

    neptune::core::ModuleRunner runner_;
    HostProbe probe_;
    ReserveHost reserve_;          // les seuls paramètres vus par l'hôte : 128 emplacements reliés aux réglages du module
    neptune::core::UndoManager undo_;
    neptune::core::settings::GlobalSettings settings_;
    neptune::core::uistate::Size modal_[2] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqualizerProcessor)
};

class EqualizerPluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit EqualizerPluginEditor (EqualizerProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    EqualizerProcessor& processor_;
    neptune::core::i18n::Translator tr_;
    void buildContent();   // (re)crée l'interface : au démarrage et quand le thème, le contraste ou la langue changent
    std::unique_ptr<NovaEqualizerEditor> content_;
    juce::ComponentBoundsConstrainer constrainer_;
};
