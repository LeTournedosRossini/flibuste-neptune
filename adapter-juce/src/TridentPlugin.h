#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <memory>
#include <vector>
#include "HostProbe.h"
#include "NovaEditor.h"
#include "ParamHost.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Settings.h"
#include "neptune/core/Theme.h"
#include "neptune/core/Trident.h"
#include "neptune/core/Undo.h"

// « Trident » : un séparateur à trois chemins (parallèle ou multibande) qui héberge les effets de la suite sans hôte CLAP. Le DSP et l'état sont dans le cœur
// (neptune::core::trident::Trident) ; ici : les paramètres de l'hôte (réglages de Trident et 32 macros), la latence totale redéclarée à l'hôte, l'interface NOVA.
class TridentProcessor : public juce::AudioProcessor, private juce::Timer,
                         public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                         public ParamHost
{
public:
    TridentProcessor();
    ~TridentProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Trident"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ParamHost : les réglages de Trident
    neptune::core::ParameterSet& parameters() override { return trident_.params(); }
    void uiBeginEdit (neptune::core::ParamId) override;
    void uiSetNormalised (neptune::core::ParamId, double) override;
    void uiEndEdit (neptune::core::ParamId) override;
    void uiResetToDefault (neptune::core::ParamId) override;
    bool uiUndo() override;
    bool uiRedo() override;
    std::string uiText (std::string_view key) const override;
    // Réserve d'automation : les réglages propres de Trident sont déjà automatisables directement ; ceux des effets passent par les emplacements.
    int reserveCount() const override { return neptune::core::trident::kAssignables; }
    std::string reserveSlotName (int slot) const override;
    bool reserveLocked (int slot) const override { return trident_.reserve().locked (slot); }
    void uiReserveReleaseSlot (int slot) override;
    void uiReserveSetLocked (int slot, bool locked) override;
    // Pour les hôtes des effets (chemin, emplacement d'effet) : relier, libérer.
    int reserveAssign (int path, int slot, neptune::core::ParamId, int reserveSlot, bool replace);
    void refreshReserveNames();   // le nom de chaque emplacement vu par l'hôte : « 12 · Chemin 1.2 Compresseur · Seuil »
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0]; }
    void setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s) override { modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0] = s; }

    neptune::core::trident::Trident& trident() { return trident_; }
    neptune::core::UndoManager& undo() { return undo_; }
    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    void applySettings (const neptune::core::settings::GlobalSettings&);   // met à jour et enregistre les réglages globaux de la suite (thème, contraste, langue)

    // Structure (fil de messages) : pose, retire un effet ; avertit l'hôte que l'état a changé.
    void setEffect (int path, int slot, const std::string& type);
    void notifyStateChanged();

private:
    static BusesProperties makeBuses();
    void timerCallback() override;   // latence totale redéclarée à l'hôte, effets retirés libérés
    int declaredLatency_ = -1;

    neptune::core::trident::Trident trident_;
    HostProbe probe_;
    std::vector<int> hostIndex_;
    neptune::core::UndoManager undo_;
    neptune::core::settings::GlobalSettings settings_;
    neptune::core::uistate::Size modal_[2] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TridentProcessor)
};

class TridentPluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit TridentPluginEditor (TridentProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void buildContent();
    TridentProcessor& processor_;
    neptune::core::i18n::Translator tr_;
    std::unique_ptr<nova::EditorBase> content_;
    juce::ComponentBoundsConstrainer constrainer_;
};
