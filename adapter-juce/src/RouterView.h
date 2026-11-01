#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>
#include <vector>
#include "ParamSearchPanel.h"
#include "PluginProcessor.h"
#include "neptune/core/I18n.h"
#include "neptune/core/NoteRouter.h"
#include "neptune/core/Theme.h"

// Onglet ROUTEUR : le routeur de notes (voir core/NoteRouter.h). Une règle par ligne : en service, nom, canal source, étendue de touches,
// canal de destination, transposition, « ajoute » (accouplement) ou « à la place » (division de clavier), condition logique sur des tirants.
// Chaque changement est un geste annulable (réglage global du mixeur).
class RouterView : public juce::Component, private juce::Timer
{
public:
    RouterView (NeptuneProcessor&, const neptune::core::theme::Palette&, const neptune::core::i18n::Translator&);
    ~RouterView() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class RuleRow;
    class StepPanel;
    // Fenêtre de réglage d'une valeur entière (touche ou transposition) : boutons à pas, champ texte, OK / Annuler.
    void askValue (const juce::String& title, int value, int lo, int hi, bool noteNames, int bigStep, std::function<void (int)> onOk);

    void timerCallback() override;
    void reload();
    void commit();
    void rebuildRows();
    void layoutRows();
    void addRule();
    void removeRule (std::size_t index);
    void editCondition (std::size_t index, juce::Component& anchor);
    void addOtherTerm (std::size_t index);
    juce::String conditionSummary (const neptune::core::router::Rule&) const;
    juce::String paramName (neptune::core::ParamId) const;
    juce::String text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }
    juce::Colour colour (neptune::core::theme::Token) const;

    NeptuneProcessor& processor_;
    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    neptune::core::router::Config config_, committed_;
    bool updating_ = false;

    juce::Label title_, hint_;
    std::array<juce::Label, 8> head_;
    juce::TextButton addButton_;
    juce::Viewport view_;
    juce::Component holder_;
    std::vector<std::unique_ptr<RuleRow>> rows_;
    std::unique_ptr<ParamSearchPanel> search_;
    std::unique_ptr<StepPanel> step_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RouterView)
};
