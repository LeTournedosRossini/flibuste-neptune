#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "ParamHost.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Theme.h"
#include "neptune/core/UiState.h"

// Modale d'un paramètre (spec-modules.md l.22) : champ texte (saisie avec unités, comme l'hôte), slider étendu,
// remise au défaut. Redimensionnable ; la taille est mémorisée par type de terminal (souris / tactile).
// Superposée à la fenêtre du plugin (pas de fenêtre système : fiable dans un hôte).
class KnobModal : public juce::Component, private juce::Timer
{
public:
    KnobModal (ParamHost&, neptune::core::ParamId, neptune::core::uistate::Terminal, const neptune::core::theme::Palette&,
               const neptune::core::i18n::Translator&, std::function<void()> onClose);
    ~KnobModal() override { stopTimer(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // un clic hors du panneau ferme
    bool keyPressed (const juce::KeyPress&) override;
    void parentSizeChanged() override;

    // « Ajouter à la Façade » : bouton dans la barre de titre (Neptune seulement) ; `onPick` reçoit le type de widget choisi
    // (0 potard, 1 bouton, 3 menu : valeurs de neptune::core::facade::WidgetType).
    void setFacadeAdd (std::function<void (int widgetType)> onPick);

    // Taille actuelle du panneau, pour la mémoriser.
    neptune::core::uistate::Size panelSize() const { return { panel_.getWidth(), panel_.getHeight() }; }
    neptune::core::uistate::Terminal terminal() const { return terminal_; }

private:
    class Panel : public juce::Component
    {
    public:
        explicit Panel (KnobModal& o) : owner_ (o) {}
        void paint (juce::Graphics& g) override { owner_.paintPanel (g, getLocalBounds().toFloat()); }
        void resized() override { owner_.layoutPanel(); }
    private:
        KnobModal& owner_;
    };

    void paintPanel (juce::Graphics&, juce::Rectangle<float>);
    void layoutPanel();
    void timerCallback() override;
    void commitText();
    void refreshFromParameter (bool force);
    void refreshAutoButton();
    juce::Colour colour (neptune::core::theme::Token t) const;
    void centrePanel();

    ParamHost& processor_;
    neptune::core::ParamId id_;
    std::size_t index_;
    neptune::core::uistate::Terminal terminal_;
    neptune::core::theme::Palette palette_;
    std::function<void()> onClose_;

    Panel panel_ { *this };
    juce::Label title_;
    juce::TextEditor text_;
    juce::Slider slider_ { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::TextButton resetButton_, closeButton_, facadeButton_, autoButton_;   // autoButton_ : réserve d'automation (plugins qui en ont une)
    std::function<void (int)> onFacadeAdd_;
    const neptune::core::i18n::Translator* tr_ = nullptr;
    juce::ComponentBoundsConstrainer constrainer_;
    juce::ResizableCornerComponent resizer_ { &panel_, &constrainer_ };
    std::uint32_t lastVersion_ = 0;
    bool sliderDragging_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobModal)
};
