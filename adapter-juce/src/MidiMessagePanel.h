#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "neptune/core/I18n.h"
#include "neptune/core/MidiSend.h"
#include "neptune/core/Theme.h"

// Éditeur d'un message MIDI : type, canal, valeurs (ou SysEx en hexadécimal), destination (Neptune, hôte, les deux). Sert à deux endroits :
//  - la fenêtre d'envoi libre (barre du haut) : « Envoyer » part tout de suite et l'historique garde la trace de chaque envoi ;
//  - le widget « Message MIDI » de la Façade : « OK » enregistre le message dans le widget (« Envoyer » l'essaie sans rien enregistrer).
class MidiMessagePanel : public juce::Component
{
public:
    struct Settings
    {
        neptune::core::midisend::Message message;
        int target = 1;   // 1 Neptune, 2 hôte, 3 les deux
        bool fader = false;
    };

    // `widgetMode` : faux = fenêtre libre (Envoyer, Fermer, historique) ; vrai = éditeur de widget (OK, Annuler, case « curseur »).
    MidiMessagePanel (const neptune::core::theme::Palette& palette, const neptune::core::i18n::Translator& tr, bool widgetMode, const Settings& initial)
        : palette_ (palette), tr_ (tr), widgetMode_ (widgetMode)
    {
        using namespace neptune::core::midisend;
        title_.setText (text (widgetMode ? "midi.widget_title" : "midi.send_title"), juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
        addAndMakeVisible (title_);
        const char* kinds[numKinds] = { "midi.kind_note_on", "midi.kind_note_off", "midi.kind_cc", "midi.kind_program", "midi.kind_bend", "midi.kind_pressure", "midi.kind_poly", "midi.kind_sysex" };
        for (int i = 0; i < numKinds; ++i) kind_.addItem (text (kinds[i]), i + 1);
        kind_.setSelectedId (static_cast<int> (initial.message.kind) + 1, juce::dontSendNotification);
        kind_.onChange = [this] { updateLabels(); };
        addAndMakeVisible (kind_);
        for (int c = 1; c <= 16; ++c) channel_.addItem (text ("midi.channel") + " " + juce::String (c), c);
        channel_.setSelectedId (initial.message.channel, juce::dontSendNotification);
        addAndMakeVisible (channel_);
        for (auto* e : { &data1_, &data2_ })
        {
            e->setInputRestrictions (5, "0123456789");
            e->setFont (juce::FontOptions (18.0f));
            addAndMakeVisible (*e);
        }
        data1_.setText (juce::String (initial.message.data1), false);
        data2_.setText (juce::String (initial.message.data2), false);
        for (auto* l : { &label1_, &label2_, &targetLabel_ })
        {
            l->setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
            addAndMakeVisible (*l);
        }
        sysex_.setMultiLine (true, true);
        sysex_.setReturnKeyStartsNewLine (true);
        sysex_.setFont (juce::FontOptions (17.0f));
        sysex_.setText (juce::String::fromUTF8 (initial.message.sysex.c_str()), false);
        addAndMakeVisible (sysex_);
        targetLabel_.setText (text ("midi.target"), juce::dontSendNotification);
        target_.addItem (text ("midi.target_neptune"), 1);
        target_.addItem (text ("midi.target_host"), 2);
        target_.addItem (text ("midi.target_both"), 3);
        target_.setSelectedId (juce::jlimit (1, 3, initial.target), juce::dontSendNotification);
        addAndMakeVisible (target_);
        if (widgetMode)
        {
            fader_.setButtonText (text ("midi.fader"));
            fader_.setToggleState (initial.fader, juce::dontSendNotification);
            fader_.setColour (juce::ToggleButton::textColourId, colour (neptune::core::theme::Token::Text));
            addAndMakeVisible (fader_);
        }
        hint_.setFont (juce::FontOptions (13.0f));
        hint_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::TextDim));
        hint_.setText (text (widgetMode ? "midi.widget_hint" : "midi.send_hint"), juce::dontSendNotification);
        addAndMakeVisible (hint_);
        error_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::KnobHot));
        addAndMakeVisible (error_);
        sendButton_.setButtonText (text ("midi.send"));
        sendButton_.onClick = [this] { send(); };
        addAndMakeVisible (sendButton_);
        if (widgetMode)
        {
            okButton_.setButtonText (text ("facade.ok"));
            okButton_.onClick = [this] { apply(); };
            cancelButton_.setButtonText (text ("facade.picker_cancel"));
            cancelButton_.onClick = [this] { if (onClose) onClose(); };
            addAndMakeVisible (okButton_);
            addAndMakeVisible (cancelButton_);
        }
        else
        {
            cancelButton_.setButtonText (text ("modal.close"));
            cancelButton_.onClick = [this] { if (onClose) onClose(); };
            addAndMakeVisible (cancelButton_);
            history_.setMultiLine (true, true);
            history_.setReadOnly (true);
            history_.setCaretVisible (false);
            history_.setFont (juce::FontOptions (14.0f));
            addAndMakeVisible (history_);
        }
        updateLabels();
    }

    // Envoi d'un message (octets, destinations) : rend vrai si la file l'a accepté.
    std::function<bool (const std::vector<std::uint8_t>&, int targets)> onSend;
    std::function<void (const Settings&)> onApply;   // mode widget : OK
    std::function<void()> onClose;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (colour (neptune::core::theme::Token::BgApp).withAlpha (1.0f));   // le fond du thème est translucide : sans ce dessous opaque, la fenêtre laisse voir le Mixeur
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (colour (neptune::core::theme::Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (colour (neptune::core::theme::Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (18);
        title_.setBounds (r.removeFromTop (32));
        r.removeFromTop (6);
        auto row = r.removeFromTop (34);
        kind_.setBounds (row.removeFromLeft (220));
        row.removeFromLeft (10);
        channel_.setBounds (row.removeFromLeft (150));
        r.removeFromTop (8);
        const bool sysex = currentKind() == neptune::core::midisend::Kind::SysEx;
        if (! sysex)
        {
            auto v1 = r.removeFromTop (34);
            label1_.setBounds (v1.removeFromLeft (230));
            data1_.setBounds (v1.removeFromLeft (110));
            r.removeFromTop (6);
            auto v2 = r.removeFromTop (34);
            label2_.setBounds (v2.removeFromLeft (230));
            data2_.setBounds (v2.removeFromLeft (110));
            r.removeFromTop (8);
        }
        else
        {
            sysex_.setBounds (r.removeFromTop (110));
            r.removeFromTop (8);
        }
        auto t = r.removeFromTop (34);
        targetLabel_.setBounds (t.removeFromLeft (120));
        target_.setBounds (t.removeFromLeft (260));
        if (widgetMode_) { r.removeFromTop (6); fader_.setBounds (r.removeFromTop (28)); }
        r.removeFromTop (6);
        hint_.setBounds (r.removeFromTop (36));
        error_.setBounds (r.removeFromTop (24));
        auto buttons = r.removeFromBottom (44);
        r.removeFromBottom (8);
        if (! widgetMode_) history_.setBounds (r);
        cancelButton_.setBounds (buttons.removeFromRight (140));
        buttons.removeFromRight (10);
        if (widgetMode_) { okButton_.setBounds (buttons.removeFromRight (140)); buttons.removeFromRight (10); }
        sendButton_.setBounds (buttons.removeFromRight (160));
    }

private:
    neptune::core::midisend::Kind currentKind() const { return static_cast<neptune::core::midisend::Kind> (std::max (0, kind_.getSelectedId() - 1)); }
    juce::String text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }
    juce::Colour colour (neptune::core::theme::Token t) const
    {
        const auto& c = palette_.color (t);
        return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
    }
    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (620, juce::jmin (widgetMode_ ? 520 : 640, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    void updateLabels()
    {
        using neptune::core::midisend::Kind;
        const auto k = currentKind();
        const char* a = "midi.v_none";
        const char* b = "midi.v_none";
        switch (k)
        {
            case Kind::NoteOn: case Kind::NoteOff: a = "midi.v_note"; b = "midi.v_velocity"; break;
            case Kind::ControlChange: a = "midi.v_controller"; b = "midi.v_value"; break;
            case Kind::ProgramChange: a = "midi.v_program"; break;
            case Kind::PitchBend: a = "midi.v_bend"; break;
            case Kind::ChannelPressure: a = "midi.v_pressure"; break;
            case Kind::PolyPressure: a = "midi.v_note"; b = "midi.v_pressure"; break;
            case Kind::SysEx: break;
        }
        label1_.setText (text (a), juce::dontSendNotification);
        label2_.setText (text (b), juce::dontSendNotification);
        data2_.setEnabled (std::string (b) != "midi.v_none");
        const bool sysex = k == Kind::SysEx;
        sysex_.setVisible (sysex);
        for (auto* c : std::initializer_list<juce::Component*> { &label1_, &label2_, &data1_, &data2_ }) c->setVisible (! sysex);
        error_.setText ({}, juce::dontSendNotification);
        resized();
    }

    Settings read() const
    {
        Settings s;
        s.message.kind = currentKind();
        s.message.channel = std::max (1, channel_.getSelectedId());
        s.message.data1 = data1_.getText().getIntValue();
        s.message.data2 = data2_.getText().getIntValue();
        s.message.sysex = sysex_.getText().toStdString();
        s.message = neptune::core::midisend::sanitised (s.message);
        s.target = std::max (1, target_.getSelectedId());
        s.fader = fader_.getToggleState();
        return s;
    }

    void send()
    {
        const auto s = read();
        std::string why;
        const auto bytes = neptune::core::midisend::encode (s.message, &why);
        if (bytes.empty()) { error_.setText (juce::String::fromUTF8 (why.c_str()), juce::dontSendNotification); return; }
        error_.setText ({}, juce::dontSendNotification);
        const bool ok = onSend && onSend (bytes, s.target);
        if (! widgetMode_)
        {
            const auto line = juce::Time::getCurrentTime().formatted ("%H:%M:%S") + "  " + juce::String::fromUTF8 (neptune::core::midisend::describe (s.message).c_str())
                            + "  [" + juce::String::fromUTF8 (neptune::core::midisend::toHex (bytes).c_str()) + "]" + (ok ? juce::String() : "  " + text ("midi.queue_full"));
            history_.moveCaretToEnd();
            history_.insertTextAtCaret (line + "\n");
        }
    }

    void apply()
    {
        const auto s = read();
        std::string why;
        if (neptune::core::midisend::encode (s.message, &why).empty()) { error_.setText (juce::String::fromUTF8 (why.c_str()), juce::dontSendNotification); return; }
        if (onApply) onApply (s);
        if (onClose) onClose();
    }

    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    bool widgetMode_;
    juce::Label title_, label1_, label2_, targetLabel_, hint_, error_;
    juce::ComboBox kind_, channel_, target_;
    juce::TextEditor data1_, data2_, sysex_, history_;
    juce::ToggleButton fader_;
    juce::TextButton sendButton_, okButton_, cancelButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMessagePanel)
};
