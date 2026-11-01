#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "ParamFilterBar.h"
#include "neptune/core/I18n.h"
#include "neptune/core/ParamGroups.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/Theme.h"

// Recherche d'un paramètre par son nom (filtre à mots, la liste commence sur ceux de la piste) : un calque plein cadre avec une liste,
// « Choisir » (double-clic ou Entrée) et « Annuler ». `continuousOnly` : seuls les paramètres continus, modulables et visibles.
class ParamSearchPanel : public juce::Component, private juce::ListBoxModel
{
public:
    ParamSearchPanel (const neptune::core::ParameterSet& set, const neptune::core::theme::Palette& palette, const neptune::core::i18n::Translator& tr,
                      int track, bool continuousOnly, std::function<void (neptune::core::ParamId)> onChoose, std::function<void()> onClose,
                      std::vector<std::pair<neptune::core::ParamId, std::string>> extras = {}, bool showScope = true)
        : palette_ (palette), tr_ (tr), onChoose_ (std::move (onChoose)), onClose_ (std::move (onClose))
    {
        for (std::size_t i = 0; i < set.size(); ++i)
        {
            const auto& s = set.spec (i);
            if (s.hidden) continue;
            if (continuousOnly && ! s.modulatable) continue;
            entries_.push_back ({ s.id, juce::String::fromUTF8 (s.name.c_str()), neptune::core::paramgroups::classify (s.name) });
        }
        for (const auto& [id, name] : extras)   // paramètres des modules en insert de la piste : famille « Inserts » de cette piste
        {
            neptune::core::paramgroups::Group g;
            g.scope = neptune::core::paramgroups::Scope::Track;
            g.index = track;
            g.family = neptune::core::paramgroups::Family::Inserts;
            entries_.push_back ({ id, juce::String::fromUTF8 (name.c_str()), g });
        }
        title_.setText (text ("modsearch.title"), juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
        addAndMakeVisible (title_);
        hint_.setText (text ("modsearch.hint"), juce::dontSendNotification);
        hint_.setFont (juce::FontOptions (13.0f));
        hint_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::TextDim));
        addAndMakeVisible (hint_);
        {
            std::vector<neptune::core::paramgroups::Group> groups;
            for (const auto& e : entries_) groups.push_back (e.group);
            bar_ = std::make_unique<ParamFilterBar> (groups, palette_, tr_, track, [this] { refilter(); }, showScope);
            addAndMakeVisible (*bar_);
        }
        filter_.setFont (juce::FontOptions (18.0f));
        filter_.onTextChange = [this] { refilter(); };
        filter_.onReturnKey = [this] { choose (list_.getSelectedRow()); };
        filter_.onEscapeKey = [this] { if (onClose_) onClose_(); };
        addAndMakeVisible (filter_);
        list_.setModel (this);
        list_.setRowHeight (26);
        list_.setColour (juce::ListBox::backgroundColourId, colour (neptune::core::theme::Token::BgApp));
        addAndMakeVisible (list_);
        cancel_.setButtonText (text ("facade.picker_cancel"));
        cancel_.onClick = [this] { if (onClose_) onClose_(); };
        addAndMakeVisible (cancel_);
        refilter();
    }

    void visibilityChanged() override { if (isVisible()) filter_.grabKeyboardFocus(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (colour (neptune::core::theme::Token::BgApp).withAlpha (1.0f));   // dessous opaque : le fond du thème est translucide
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (colour (neptune::core::theme::Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (colour (neptune::core::theme::Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (16);
        title_.setBounds (r.removeFromTop (30));
        hint_.setBounds (r.removeFromTop (36));
        filter_.setBounds (r.removeFromTop (32));
        r.removeFromTop (6);
        bar_->setBounds (r.removeFromTop (bar_->preferredHeight (r.getWidth())));
        r.removeFromTop (8);
        auto bottom = r.removeFromBottom (32);
        r.removeFromBottom (8);
        list_.setBounds (r);
        cancel_.setBounds (bottom.removeFromRight (120));
    }

private:
    struct Entry { neptune::core::ParamId id; juce::String name; neptune::core::paramgroups::Group group; };

    juce::String text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }
    juce::Colour colour (neptune::core::theme::Token t) const
    {
        const auto& c = palette_.color (t);
        return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
    }
    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (560, juce::jmin (700, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    void refilter()
    {
        shown_.clear();
        const auto words = juce::StringArray::fromTokens (filter_.getText().trim(), " ", "");
        for (std::size_t i = 0; i < entries_.size(); ++i)
        {
            bool ok = bar_ == nullptr || bar_->accepts (entries_[i].group);
            for (const auto& w : words)
                if (w.isNotEmpty() && ! entries_[i].name.containsIgnoreCase (w)) { ok = false; break; }
            if (ok) shown_.push_back (i);
        }
        list_.updateContent();
        list_.selectRow (shown_.empty() ? -1 : 0);
        list_.repaint();
    }

    void choose (int row)
    {
        if (row < 0 || row >= static_cast<int> (shown_.size())) return;
        if (onChoose_) onChoose_ (entries_[shown_[static_cast<std::size_t> (row)]].id);
        if (onClose_) onClose_();
    }

    int getNumRows() override { return static_cast<int> (shown_.size()); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= static_cast<int> (shown_.size())) return;
        if (selected) { g.setColour (colour (neptune::core::theme::Token::Accent).withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
        g.setColour (colour (neptune::core::theme::Token::Text));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (entries_[shown_[static_cast<std::size_t> (row)]].name, 8, 0, w - 16, h, juce::Justification::centredLeft);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { choose (row); }

    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    std::function<void (neptune::core::ParamId)> onChoose_;
    std::function<void()> onClose_;
    std::vector<Entry> entries_;
    std::vector<std::size_t> shown_;
    juce::Label title_, hint_;
    juce::TextEditor filter_;
    std::unique_ptr<ParamFilterBar> bar_;
    juce::ListBox list_;
    juce::TextButton cancel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamSearchPanel)
};
