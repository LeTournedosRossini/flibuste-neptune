#include "KnobModal.h"
#include "ReserveMenu.h"

using neptune::core::theme::Token;

KnobModal::KnobModal (ParamHost& p, neptune::core::ParamId id, neptune::core::uistate::Terminal terminal,
                      const neptune::core::theme::Palette& palette, const neptune::core::i18n::Translator& tr,
                      std::function<void()> onClose)
    : processor_ (p), id_ (id), index_ (p.parameters().indexOf (id)), terminal_ (terminal), palette_ (palette), onClose_ (std::move (onClose))
{
    setWantsKeyboardFocus (true);
    addAndMakeVisible (panel_);

    const auto& set = processor_.parameters();
    if (index_ != neptune::core::ParameterSet::npos)
        title_.setText (juce::String::fromUTF8 (set.spec (index_).name.c_str()), juce::dontSendNotification);
    title_.setColour (juce::Label::textColourId, colour (Token::Text));
    title_.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    panel_.addAndMakeVisible (title_);

    text_.setMultiLine (false);
    text_.setSelectAllWhenFocused (true);
    text_.setColour (juce::TextEditor::backgroundColourId, colour (Token::BgCard));
    text_.setColour (juce::TextEditor::textColourId, colour (Token::Text));
    text_.setColour (juce::TextEditor::outlineColourId, colour (Token::CtlBd));
    text_.setColour (juce::TextEditor::focusedOutlineColourId, colour (Token::Accent));
    text_.setFont (juce::Font (juce::FontOptions (18.0f)));
    text_.onReturnKey = [this] { commitText(); };
    text_.onFocusLost = [this] { refreshFromParameter (true); };
    text_.onEscapeKey = [this] { if (onClose_) onClose_(); };
    panel_.addAndMakeVisible (text_);

    slider_.setRange (0.0, 1.0, 0.0);
    slider_.setColour (juce::Slider::trackColourId, colour (Token::Accent));
    slider_.setColour (juce::Slider::backgroundColourId, colour (Token::Grid));
    slider_.setColour (juce::Slider::thumbColourId, colour (Token::Handle));
    slider_.onDragStart = [this] { sliderDragging_ = true; processor_.uiBeginEdit (id_); };
    slider_.onValueChange = [this]
    {
        if (! sliderDragging_) return;
        processor_.uiSetNormalised (id_, slider_.getValue());
        // le champ de saisie garde souvent le focus clavier : on le met à jour ici, sans attendre la minuterie, pour que la valeur suive le fader en direct
        text_.setText (juce::String::fromUTF8 (processor_.parameters().toText (id_).c_str()), false);
    };
    slider_.onDragEnd = [this] { sliderDragging_ = false; processor_.uiEndEdit (id_); refreshFromParameter (true); };
    panel_.addAndMakeVisible (slider_);

    resetButton_.setButtonText (juce::String::fromUTF8 (std::string (tr.tr ("knob.reset")).c_str()));
    resetButton_.onClick = [this] { processor_.uiResetToDefault (id_); refreshFromParameter (true); };
    panel_.addAndMakeVisible (resetButton_);
    closeButton_.setButtonText (juce::String::fromUTF8 (std::string (tr.tr ("modal.close")).c_str()));
    closeButton_.onClick = [this] { if (onClose_) onClose_(); };
    panel_.addAndMakeVisible (closeButton_);
    // Réserve d'automation : le même choix que le clic droit, accessible au doigt (appui long sur le potard ouvre cette modale)
    if (processor_.reserveCount() > 0 && (processor_.reserveCanAssign (id_) || processor_.reserveDirect (id_)))
    {
        autoButton_.onClick = [this]
        {
            if (processor_.reserveCanAssign (id_)) reservemenu::show (processor_, id_, autoButton_, [this] { refreshAutoButton(); });
        };
        panel_.addAndMakeVisible (autoButton_);
        refreshAutoButton();
    }
    tr_ = &tr;

    auto size = processor_.modalSize (terminal_);
    // Taille jamais mémorisée (plugin séparé : l'hôte ne fournit rien) : valeur par défaut, sinon le panneau serait invisible derrière le voile.
    if (size.width < neptune::core::uistate::minModalSide || size.height < 140) size = { terminal_ == neptune::core::uistate::Terminal::Touch ? 560 : 460, terminal_ == neptune::core::uistate::Terminal::Touch ? 300 : 260 };
    constrainer_.setMinimumSize (neptune::core::uistate::minModalSide, 140);
    constrainer_.setMaximumSize (neptune::core::uistate::maxModalSide, neptune::core::uistate::maxModalSide);
    panel_.setSize (size.width, size.height);
    panel_.addAndMakeVisible (resizer_);

    refreshFromParameter (true);
    startTimerHz (20);
}

juce::Colour KnobModal::colour (Token t) const
{
    const auto& c = palette_.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

void KnobModal::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.45f)); // voile derrière le panneau
}

void KnobModal::paintPanel (juce::Graphics& g, juce::Rectangle<float> b)
{
    g.setColour (colour (Token::BgCard));
    g.fillRoundedRectangle (b, 10.0f);
    g.setColour (colour (Token::Border));
    g.drawRoundedRectangle (b.reduced (0.5f), 10.0f, 1.0f);
}

void KnobModal::centrePanel()
{
    auto b = panel_.getBounds().withSizeKeepingCentre (panel_.getWidth(), panel_.getHeight());
    const auto area = getLocalBounds();
    b.setSize (juce::jmin (b.getWidth(), area.getWidth()), juce::jmin (b.getHeight(), area.getHeight()));
    panel_.setBounds (area.getCentreX() - b.getWidth() / 2, area.getCentreY() - b.getHeight() / 2, b.getWidth(), b.getHeight());
}

void KnobModal::resized()
{
    centrePanel();
    layoutPanel();
}

void KnobModal::parentSizeChanged() { centrePanel(); }

void KnobModal::setFacadeAdd (std::function<void (int)> onPick)
{
    onFacadeAdd_ = std::move (onPick);
    if (! onFacadeAdd_ || tr_ == nullptr) return;
    auto label = [this] (const char* key) { return juce::String::fromUTF8 (std::string (tr_->tr (key)).c_str()); };
    facadeButton_.setButtonText (label ("facade.modal_add"));
    facadeButton_.onClick = [modal = this, label]
    {
        juce::PopupMenu menu;
        menu.addItem (1, label ("facade.type_knob"));
        menu.addItem (2, label ("facade.type_button"));
        menu.addItem (3, label ("facade.type_menu"));
        menu.setLookAndFeel (&modal->getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&modal->facadeButton_), [safe = juce::Component::SafePointer<KnobModal> (modal)] (int choice)
        {
            if (safe == nullptr || choice <= 0 || ! safe->onFacadeAdd_) return;
            safe->onFacadeAdd_ (choice == 1 ? 0 : choice == 2 ? 1 : 3);   // WidgetType::Knob, Button, Menu
        });
    };
    panel_.addAndMakeVisible (facadeButton_);
    layoutPanel();
}

void KnobModal::layoutPanel()
{
    // Cibles tactiles : champs et boutons au moins 44 px de haut.
    const int rowH = 44;
    auto r = panel_.getLocalBounds().reduced (16);
    auto titleRow = r.removeFromTop (28);
    if (facadeButton_.isVisible()) facadeButton_.setBounds (titleRow.removeFromRight (120));
    title_.setBounds (titleRow);
    r.removeFromTop (8);
    text_.setBounds (r.removeFromTop (rowH));
    r.removeFromTop (12);
    slider_.setBounds (r.removeFromTop (rowH));
    auto buttons = r.removeFromBottom (rowH);
    closeButton_.setBounds (buttons.removeFromRight (120));
    buttons.removeFromRight (8);
    resetButton_.setBounds (buttons.removeFromRight (180));
    if (autoButton_.isVisible())
    {
        buttons.removeFromRight (8);
        autoButton_.setBounds (buttons.removeFromRight (std::min (buttons.getWidth(), 260)));
    }
    resizer_.setBounds (panel_.getWidth() - 20, panel_.getHeight() - 20, 20, 20);
}

void KnobModal::mouseDown (const juce::MouseEvent& e)
{
    if (! panel_.getBounds().contains (e.getPosition()) && onClose_) onClose_();
}

bool KnobModal::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (onClose_) onClose_();
        return true;
    }
    return false;
}

void KnobModal::commitText()
{
    if (index_ == neptune::core::ParameterSet::npos) return;
    double plain = 0.0;
    if (processor_.parameters().fromText (id_, text_.getText().toStdString(), plain))
    {
        const auto& spec = processor_.parameters().spec (index_);
        processor_.uiBeginEdit (id_);
        processor_.uiSetNormalised (id_, spec.toNormalized (plain));
        processor_.uiEndEdit (id_);
    }
    refreshFromParameter (true); // texte illisible : on remet la valeur courante
}

void KnobModal::refreshFromParameter (bool force)
{
    if (index_ == neptune::core::ParameterSet::npos) return;
    const auto& set = processor_.parameters();
    const auto v = set.versionAt (index_);
    if (! force && v == lastVersion_) return;
    lastVersion_ = v;
    if (! sliderDragging_) slider_.setValue (set.getNormalizedAt (index_), juce::dontSendNotification);
    if (force || ! text_.hasKeyboardFocus (false))
        text_.setText (juce::String::fromUTF8 (set.toText (id_).c_str()), false);
}

void KnobModal::refreshAutoButton()
{
    if (! autoButton_.isVisible() && autoButton_.getParentComponent() == nullptr) return;
    if (! processor_.reserveCanAssign (id_))
    {
        autoButton_.setButtonText (reservemenu::U ("Automatisable directement dans l'hôte"));
        autoButton_.setEnabled (false);
        return;
    }
    const int k = processor_.reserveSlotOf (id_);
    autoButton_.setButtonText (k >= 0 ? reservemenu::U ("Automatisé : emplacement ") + juce::String (k + 1) : reservemenu::U ("Automatiser..."));
}

void KnobModal::timerCallback()
{
    refreshFromParameter (false);
    refreshAutoButton();
}
