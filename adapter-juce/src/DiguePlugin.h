#pragma once
#include <vector>
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <memory>
#include "HostProbe.h"
#include "ParamHost.h"
#include "NovaDigueEditor.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Settings.h"
#include "neptune/core/StockModule.h"
#include "neptune/core/Theme.h"
#include "neptune/core/Undo.h"

// « Digue » : limiteur brickwall à anticipation (Quality / Low Latency), publié comme plugin CLAP effet indépendant (série NOVA).
// Le DSP est celui du module stock LimiterModule ; ici : les paramètres de l'hôte et la latence (durée de l'anticipation), redéclarée à l'hôte quand
// le mode ou le réglage Lookahead change.
class DigueProcessor : public juce::AudioProcessor, private juce::Timer,
                             public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                             public ParamHost
{
public:
    DigueProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Digue"; }
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
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0]; }
    void setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s) override { modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0] = s; }

    neptune::core::ModuleRunner& runner() { return runner_; }
    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    void applySettings (const neptune::core::settings::GlobalSettings&);   // met à jour et enregistre les réglages globaux de la suite (thème, contraste, langue)

private:
    static BusesProperties makeBuses();
    void timerCallback() override;   // thread de message : la latence déclarée suit le suréchantillonnage
    int declaredLatency_ = -1;

    neptune::core::ModuleRunner runner_;
    HostProbe probe_;
    std::vector<int> hostIndex_;   // indice de chaque réglage dans la liste de l'hôte ; -1 : réglage caché (affichage), non déclaré à l'hôte
    neptune::core::UndoManager undo_;
    neptune::core::settings::GlobalSettings settings_;
    neptune::core::uistate::Size modal_[2] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DigueProcessor)
};

class DiguePluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit DiguePluginEditor (DigueProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    DigueProcessor& processor_;
    neptune::core::i18n::Translator tr_;
    void buildContent();   // (re)crée l'interface : au démarrage et quand le thème, le contraste ou la langue changent
    std::unique_ptr<NovaDigueEditor> content_;
    juce::ComponentBoundsConstrainer constrainer_;
};
