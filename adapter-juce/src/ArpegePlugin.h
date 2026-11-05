#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include "NovaKit.h"
#include "neptune/core/Arpeggiator.h"
#include "neptune/core/Settings.h"
#include "neptune/core/Theme.h"

// « Arpège » (nom provisoire) : arpégiateur. Reçoit des notes tenues, sort des notes calées sur la grille de l'hôte (instant exact dans le bloc) vers
// l'instrument suivant. Cœur : neptune::core::arp::Arpeggiator. Voir docs/plan-harmonie-arpegiateur.md (phase 2 ; la lecture de la carte d'accords = phase 3).
// Les messages MIDI qui ne sont pas des notes (contrôleurs, roue de hauteur...) passent sans changement.
class ArpegeProcessor : public juce::AudioProcessor
{
public:
    ArpegeProcessor();
    ~ArpegeProcessor() override = default;

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

    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    int heldCount() const noexcept { return held_.load(); }
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
    void timerCallback() override;

    ArpegeProcessor& proc_;
    nova::Skin skin_;
    juce::ComponentBoundsConstrainer constrainer_;
    std::vector<std::unique_ptr<ParamPill>> pills_;
};
