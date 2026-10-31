#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "PluginProcessor.h"
#include "NovaCompressorEditor.h"
#include "NovaChorusEditor.h"
#include "NovaDelayEditor.h"
#include "NovaMicrodecaleEditor.h"
#include "NovaConvolutionEditor.h"
#include "NovaFournaiseEditor.h"
#include "NovaEqualizerEditor.h"
#include "NovaShaperEditor.h"
#include "UtilityEditor.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Theme.h"

// Éditeur d'un insert, superposé à la fenêtre (comme l'éditeur de filtres) : titre, bypass de l'emplacement, fermeture, et
// l'interface du module. Un clic hors du panneau ou Échap ferme.
class InsertOverlay : public juce::Component, private juce::Timer
{
public:
    InsertOverlay (NeptuneProcessor&, NeptuneProcessor::StripKind, int index, int slot, const neptune::core::theme::Palette&,
                   const neptune::core::i18n::Translator&, neptune::core::theme::KnobStyle, std::function<void()> onClose);
    ~InsertOverlay() override { stopTimer(); }

    bool valid() const { return handle_.instance != nullptr; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    juce::Colour colour (neptune::core::theme::Token t) const;
    juce::String text (const char* key) const;

    NeptuneProcessor& processor_;
    NeptuneProcessor::StripKind kind_;
    int index_, slot_;
    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    std::function<void()> onClose_;
    NeptuneProcessor::InsertHandle handle_;
    std::unique_ptr<UtilityEditor> content_;
    std::unique_ptr<NovaShaperEditor> shaperContent_;
    std::unique_ptr<NovaEqualizerEditor> equalizerContent_;
    std::unique_ptr<NovaDelayEditor> delayContent_;
    std::unique_ptr<NovaChorusEditor> chorusContent_;
    std::unique_ptr<NovaMicrodecaleEditor> microContent_;
    std::unique_ptr<NovaConvolutionEditor> convContent_;
    std::unique_ptr<NovaFournaiseEditor> overdriveContent_;
    std::unique_ptr<NovaCompressorEditor> compressorContent_;
    juce::TextButton bypassButton_, closeButton_;
    juce::String title_;
    juce::Rectangle<int> panel_;
    neptune::core::ParamId bypassId_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertOverlay)
};
