#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include "neptune/core/SharedChordMap.h"

// « Essai Carte » : plugin d'essai de la carte d'accords partagée entre plugins (voir docs/plan-harmonie-arpegiateur.md). Ce n'est ni la future « Carte » ni
// le futur arpégiateur : il sert à vérifier dans Reaper que deux instances se parlent (même processus ou non), sous Linux et Windows.
//   Rôle « Écrivain » : publie une carte d'exemple (Do | Fa | Sol | La mineur, quatre temps chacun, répétée) + la tonalité de Do majeur.
//   Rôle « Lecteur »  : lit la carte avec la position de lecture de l'hôte et affiche l'accord en cours et le suivant.
// Le son passe sans changement.
class EssaiCarteProcessor : public juce::AudioProcessor
{
public:
    EssaiCarteProcessor();
    ~EssaiCarteProcessor() override;

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Essai Carte"; }
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

    juce::AudioParameterChoice* role() { return role_; }       // 0 écrivain, 1 lecteur
    juce::AudioParameterInt* channel() { return channel_; }

    // Ce que le lecteur a vu au dernier bloc (lu par l'interface)
    struct Seen
    {
        bool connected = false, hasPosition = false, hasChord = false, hasNext = false, hasKey = false;
        double beats = 0.0;
        neptune::core::chordmap::Container chord, next, key;
        std::uint64_t sequence = 0;
        int writerStatus = -1;   // écrivain : -1 fermé, sinon Status
    };
    Seen seen() const;

private:
    struct Ticker : juce::Timer
    {
        explicit Ticker (EssaiCarteProcessor& p) : owner (p) {}
        void timerCallback() override { owner.tick(); }
        EssaiCarteProcessor& owner;
    };
    void tick();   // thread de message, deux fois par seconde : ouverture, signe de vie, connexion
    void publishExample();

    juce::AudioParameterChoice* role_ = nullptr;
    juce::AudioParameterInt* channel_ = nullptr;
    neptune::core::chordmap::Writer writer_;
    neptune::core::chordmap::Reader reader_;
    int openedRole_ = -1, openedChannel_ = 0, writerStatus_ = -1;
    Ticker ticker_ { *this };

    // thread audio -> interface (verrou tournant sur un compteur : copie cohérente)
    mutable std::atomic<std::uint32_t> seenGen_ { 0 };
    Seen seenData_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EssaiCarteProcessor)
};

class EssaiCarteEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit EssaiCarteEditor (EssaiCarteProcessor&);
    ~EssaiCarteEditor() override { stopTimer(); }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { repaint(); }
    EssaiCarteProcessor& proc_;
    juce::ComboBox role_, channel_;
    juce::Label roleLabel_, channelLabel_;
};
