#include "RouterView.h"

#include <algorithm>
#include <cstdlib>
#include "neptune/core/ZoneImport.h"

using neptune::core::theme::Token;
namespace rt = neptune::core::router;

namespace
{
// Largeurs des colonnes d'une règle (la ligne de légende reprend les mêmes).
constexpr int kOnW = 34, kNameW = 150, kSrcW = 76, kKeyW = 76, kDstW = 70, kTransW = 70, kModeW = 150, kRemoveW = 34, kGap = 6;
}

// ------------------------------------------------------------------------------------------------ une règle

class RouterView::RuleRow : public juce::Component
{
public:
    RuleRow (RouterView& owner, std::size_t index) : owner_ (owner), index_ (index)
    {
        on_.setClickingTogglesState (true);
        on_.setTooltip (owner.text ("router.tip_on"));
        on_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::Accent));
        on_.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93"));
        on_.onClick = [this] { if (! updating_) { edit().on = on_.getToggleState(); owner_.commit(); } };
        addAndMakeVisible (on_);
        name_.setFont (juce::FontOptions (15.0f));
        name_.setTextToShowWhenEmpty (owner.text ("router.name_hint"), owner.colour (Token::TextDim));
        name_.onReturnKey = name_.onFocusLost = [this] { if (! updating_) { edit().name = name_.getText().toStdString(); owner_.commit(); } };
        addAndMakeVisible (name_);
        src_.addItem (owner.text ("router.src_any"), 1);
        for (int c = 1; c <= 16; ++c) src_.addItem (juce::String (c), c + 1);
        src_.onChange = [this] { if (! updating_) { edit().srcChannel = src_.getSelectedId() - 1; owner_.commit(); } };
        addAndMakeVisible (src_);
        for (int c = 1; c <= 16; ++c) dst_.addItem (juce::String (c), c);
        dst_.onChange = [this] { if (! updating_) { edit().dstChannel = dst_.getSelectedId(); owner_.commit(); } };
        addAndMakeVisible (dst_);
        auto note = [this] (juce::TextButton& b, bool low)
        {
            b.setTooltip (owner_.text (low ? "router.tip_lo" : "router.tip_hi"));
            b.onClick = [this, low]
            {
                owner_.askValue (owner_.text (low ? "router.ask_lo" : "router.ask_hi"), low ? edit().keyLo : edit().keyHi, 0, 127, true, 12,
                                 [this, low] (int v)
                                 {
                                     (low ? edit().keyLo : edit().keyHi) = v;
                                     owner_.commit();
                                     refresh (edit());
                                 });
            };
            addAndMakeVisible (b);
        };
        note (lo_, true);
        note (hi_, false);
        trans_.setTooltip (owner.text ("router.tip_transpose"));
        trans_.onClick = [this]
        {
            owner_.askValue (owner_.text ("router.ask_transpose"), edit().transpose, -48, 48, false, 12,
                             [this] (int v) { edit().transpose = v; owner_.commit(); refresh (edit()); });
        };
        addAndMakeVisible (trans_);
        mode_.addItem (owner.text ("router.mode_add"), 1);
        mode_.addItem (owner.text ("router.mode_replace"), 2);
        mode_.addItem (owner.text ("router.mode_bass"), 3);
        mode_.addItem (owner.text ("router.mode_melody"), 4);
        mode_.setTooltip (owner.text ("router.tip_mode"));
        mode_.onChange = [this]
        {
            if (updating_) return;
            const int id = mode_.getSelectedId();
            edit().replace = id == 2;
            edit().select = id == 3 ? neptune::core::router::Select::Lowest : id == 4 ? neptune::core::router::Select::Highest : neptune::core::router::Select::All;
            owner_.commit();
        };
        addAndMakeVisible (mode_);
        cond_.onClick = [this] { owner_.editCondition (index_, cond_); };
        addAndMakeVisible (cond_);
        remove_.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        remove_.setTooltip (owner.text ("router.remove"));
        remove_.onClick = [this] { owner_.removeRule (index_); };
        addAndMakeVisible (remove_);
        refresh (edit());
    }

    void paint (juce::Graphics& g) override   // une carte par règle
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (owner_.colour (Token::BgCard));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (b, 7.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 2);
        on_.setBounds (r.removeFromLeft (kOnW));
        r.removeFromLeft (kGap);
        name_.setBounds (r.removeFromLeft (kNameW));
        r.removeFromLeft (kGap);
        src_.setBounds (r.removeFromLeft (kSrcW));
        r.removeFromLeft (kGap);
        lo_.setBounds (r.removeFromLeft (kKeyW));
        r.removeFromLeft (kGap);
        hi_.setBounds (r.removeFromLeft (kKeyW));
        r.removeFromLeft (kGap);
        dst_.setBounds (r.removeFromLeft (kDstW));
        r.removeFromLeft (kGap);
        trans_.setBounds (r.removeFromLeft (kTransW));
        r.removeFromLeft (kGap);
        mode_.setBounds (r.removeFromLeft (kModeW));
        r.removeFromLeft (kGap);
        remove_.setBounds (r.removeFromRight (kRemoveW));
        r.removeFromRight (kGap);
        cond_.setBounds (r);
    }

    void refresh (const rt::Rule& r)
    {
        updating_ = true;
        on_.setToggleState (r.on, juce::dontSendNotification);
        name_.setText (juce::String::fromUTF8 (r.name.c_str()), false);
        src_.setSelectedId (r.srcChannel + 1, juce::dontSendNotification);
        dst_.setSelectedId (r.dstChannel, juce::dontSendNotification);
        auto keyText = [] (int k) { return juce::String (k) + "  " + juce::String::fromUTF8 (neptune::core::import::midiToNoteName (k, neptune::core::import::NoteConvention {}).c_str()); };
        lo_.setButtonText (keyText (r.keyLo));
        hi_.setButtonText (keyText (r.keyHi));
        trans_.setButtonText (r.transpose > 0 ? "+" + juce::String (r.transpose) : juce::String (r.transpose));
        mode_.setSelectedId (r.select == neptune::core::router::Select::Lowest ? 3 : r.select == neptune::core::router::Select::Highest ? 4 : r.replace ? 2 : 1, juce::dontSendNotification);
        cond_.setButtonText (owner_.conditionSummary (r));
        updating_ = false;
    }

private:
    rt::Rule& edit() { return owner_.config_.rules[index_]; }

    RouterView& owner_;
    std::size_t index_;
    bool updating_ = false;
    juce::TextButton on_, cond_, remove_, lo_, hi_, trans_;
    juce::TextEditor name_;
    juce::ComboBox src_, dst_, mode_;
};

// ------------------------------------------------------------------------------------------------ fenêtre de réglage d'une valeur

class RouterView::StepPanel : public juce::Component
{
public:
    StepPanel (RouterView& owner, const juce::String& title, int value, int lo, int hi, bool noteNames, int bigStep, std::function<void (int)> onOk)
        : owner_ (owner), lo_ (lo), hi_ (hi), noteNames_ (noteNames), onOk_ (std::move (onOk)), value_ (std::clamp (value, lo, hi))
    {
        title_.setText (title, juce::dontSendNotification);
        title_.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Accent));
        title_.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (title_);
        auto step = [this] (juce::TextButton& b, const juce::String& label, int delta)
        {
            b.setButtonText (label);
            b.onClick = [this, delta] { set (value_ + delta); };
            addAndMakeVisible (b);
        };
        step (bigDown_, "-" + juce::String (bigStep), -bigStep);
        step (down_, "-1", -1);
        step (up_, "+1", 1);
        step (bigUp_, "+" + juce::String (bigStep), bigStep);
        field_.setFont (juce::FontOptions (20.0f));
        field_.setJustification (juce::Justification::centred);
        field_.setInputRestrictions (4, "-+0123456789");
        field_.onReturnKey = [this] { apply(); };
        addAndMakeVisible (field_);
        name_.setJustificationType (juce::Justification::centred);
        name_.setColour (juce::Label::textColourId, owner.colour (Token::TextDim));
        addAndMakeVisible (name_);
        ok_.setButtonText (owner.text ("grp.ok"));
        ok_.onClick = [this] { apply(); };
        addAndMakeVisible (ok_);
        cancel_.setButtonText (owner.text ("mix.cancel"));
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible (cancel_);
        set (value_);
        setWantsKeyboardFocus (true);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));
        const auto box = panelBounds().toFloat();
        g.setColour (owner_.colour (Token::BgCard));
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);
    }
    void resized() override
    {
        auto r = panelBounds().reduced (14);
        title_.setBounds (r.removeFromTop (28));
        r.removeFromTop (8);
        auto row = r.removeFromTop (40);
        const int w = (row.getWidth() - 4 * 6 - 96) / 4;
        bigDown_.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        down_.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        field_.setBounds (row.removeFromLeft (96));
        row.removeFromLeft (6);
        up_.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        bigUp_.setBounds (row);
        name_.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        auto buttons = r.removeFromTop (30);
        cancel_.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);
        ok_.setBounds (buttons.removeFromRight (110));
    }
    void mouseDown (const juce::MouseEvent& e) override { if (! panelBounds().contains (e.getPosition())) close(); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { close(); return true; }
        if (k == juce::KeyPress::returnKey) { apply(); return true; }
        return false;
    }

private:
    juce::Rectangle<int> panelBounds() const { return getLocalBounds().withSizeKeepingCentre (std::min (getWidth() - 20, 420), 190); }
    void set (int v)
    {
        value_ = std::clamp (v, lo_, hi_);
        field_.setText (value_ > 0 && ! noteNames_ ? "+" + juce::String (value_) : juce::String (value_), false);
        if (noteNames_) name_.setText (juce::String::fromUTF8 (neptune::core::import::midiToNoteName (value_, neptune::core::import::NoteConvention {}).c_str()), juce::dontSendNotification);
        else name_.setText (value_ == 0 ? owner_.text ("router.no_transpose") : juce::String (std::abs (value_) / 12) + " " + owner_.text ("router.octaves") + " " + juce::String (std::abs (value_) % 12) + " " + owner_.text ("router.semitones")
                                + " " + owner_.text (value_ > 0 ? "router.up" : "router.down"), juce::dontSendNotification);
    }
    void apply()
    {
        set (field_.getText().getIntValue());
        const auto done = onOk_;
        const int v = value_;
        close();
        if (done) done (v);
    }
    void close()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<RouterView> (&owner_)] { if (safe != nullptr) safe->step_.reset(); });
    }

    RouterView& owner_;
    int lo_, hi_;
    bool noteNames_;
    std::function<void (int)> onOk_;
    int value_;
    juce::Label title_, name_;
    juce::TextButton bigDown_, down_, up_, bigUp_, ok_, cancel_;
    juce::TextEditor field_;
};

void RouterView::askValue (const juce::String& title, int value, int lo, int hi, bool noteNames, int bigStep, std::function<void (int)> onOk)
{
    if (step_ != nullptr) return;
    step_ = std::make_unique<StepPanel> (*this, title, value, lo, hi, noteNames, bigStep, std::move (onOk));
    addAndMakeVisible (*step_);
    step_->setBounds (getLocalBounds());
    step_->toFront (true);
    step_->grabKeyboardFocus();
}

// ------------------------------------------------------------------------------------------------ la vue

RouterView::RouterView (NeptuneProcessor& p, const neptune::core::theme::Palette& palette, const neptune::core::i18n::Translator& tr)
    : processor_ (p), palette_ (palette), tr_ (tr)
{
    title_.setText (text ("router.title"), juce::dontSendNotification);
    title_.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    title_.setColour (juce::Label::textColourId, colour (Token::Accent));
    addAndMakeVisible (title_);
    hint_.setText (text ("router.hint"), juce::dontSendNotification);
    hint_.setFont (juce::FontOptions (13.0f));
    hint_.setColour (juce::Label::textColourId, colour (Token::TextDim));
    addAndMakeVisible (hint_);
    const char* keys[8] = { "router.head_on", "router.head_name", "router.head_source", "router.head_lo", "router.head_hi", "router.head_dest", "router.head_transpose", "router.head_mode" };
    for (std::size_t i = 0; i < head_.size(); ++i)
    {
        head_[i].setText (text (keys[i]), juce::dontSendNotification);
        head_[i].setFont (juce::FontOptions (13.0f));
        head_[i].setColour (juce::Label::textColourId, colour (Token::TextDim));
        head_[i].setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (head_[i]);
    }
    addButton_.setButtonText (text ("router.add"));
    addButton_.onClick = [this] { addRule(); };
    addAndMakeVisible (addButton_);
    addAndMakeVisible (view_);
    view_.setViewedComponent (&holder_, false);
    view_.setScrollBarsShown (true, false);
    reload();
    startTimerHz (8);
}

RouterView::~RouterView() { stopTimer(); }

juce::Colour RouterView::colour (Token t) const
{
    const auto& c = palette_.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

void RouterView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colour (Token::PanelBg));
    g.fillRoundedRectangle (b, 10.0f);
    g.setColour (colour (Token::Pb));
    g.drawRoundedRectangle (b, 10.0f, 1.0f);
}

void RouterView::resized()
{
    auto r = getLocalBounds().reduced (14, 10);   // tout dans un conteneur arrondi
    auto head = r.removeFromTop (30);
    title_.setBounds (head.removeFromLeft (220));
    addButton_.setBounds (head.removeFromRight (160));
    hint_.setBounds (head);
    r.removeFromTop (4);
    auto legend = r.removeFromTop (20).reduced (4, 0);
    view_.setBounds (r);
    layoutRows();
    // même géométrie que RuleRow::resized()
    head_[0].setBounds (legend.removeFromLeft (kOnW));
    legend.removeFromLeft (kGap);
    head_[1].setBounds (legend.removeFromLeft (kNameW));
    legend.removeFromLeft (kGap);
    head_[2].setBounds (legend.removeFromLeft (kSrcW));
    legend.removeFromLeft (kGap);
    head_[3].setBounds (legend.removeFromLeft (kKeyW));
    legend.removeFromLeft (kGap);
    head_[4].setBounds (legend.removeFromLeft (kKeyW));
    legend.removeFromLeft (kGap);
    head_[5].setBounds (legend.removeFromLeft (kDstW));
    legend.removeFromLeft (kGap);
    head_[6].setBounds (legend.removeFromLeft (kTransW));
    legend.removeFromLeft (kGap);
    head_[7].setBounds (legend.removeFromLeft (kModeW));
    if (search_ != nullptr) search_->setBounds (getLocalBounds());
    if (step_ != nullptr) step_->setBounds (getLocalBounds());
}

void RouterView::layoutRows()
{
    const int width = std::max (700, view_.getMaximumVisibleWidth());
    int y = 0;
    for (auto& row : rows_) { row->setBounds (0, y, width, 36); y += 38; }
    holder_.setSize (width, std::max (y, 1));
}

void RouterView::reload()
{
    committed_ = processor_.routerConfig();
    config_ = committed_;
    rebuildRows();
}

void RouterView::rebuildRows()
{
    rows_.clear();
    for (std::size_t i = 0; i < config_.rules.size(); ++i)
    {
        rows_.push_back (std::make_unique<RuleRow> (*this, i));
        holder_.addAndMakeVisible (*rows_.back());
    }
    addButton_.setEnabled (config_.rules.size() < static_cast<std::size_t> (rt::kMaxRules));
    layoutRows();
}

void RouterView::commit()
{
    committed_ = rt::sanitised (config_);
    config_ = committed_;   // la liste montre ce qui est enregistré (bornes appliquées)
    processor_.uiEditRouter (committed_, "Router", "router");
}

void RouterView::timerCallback()
{
    if (! isShowing()) return;
    if (processor_.routerConfig() != committed_) reload();   // annulation, chargement d'un preset... : le routeur a changé ailleurs
}

void RouterView::addRule()
{
    if (config_.rules.size() >= static_cast<std::size_t> (rt::kMaxRules)) return;
    rt::Rule r;
    r.srcChannel = 2;
    r.dstChannel = 1;
    r.terms.push_back ({ rt::switchParam (static_cast<int> (config_.rules.size()) % rt::kSwitches), false });   // un tirant par défaut : à renommer ou changer
    config_.rules.push_back (r);
    commit();
    rebuildRows();
}

void RouterView::removeRule (std::size_t index)
{
    if (index >= config_.rules.size()) return;
    config_.rules.erase (config_.rules.begin() + static_cast<std::ptrdiff_t> (index));
    commit();
    rebuildRows();
}

juce::String RouterView::paramName (neptune::core::ParamId id) const
{
    const auto index = processor_.parameters().indexOf (id);
    if (index == neptune::core::ParameterSet::npos) return "#" + juce::String (id);
    return juce::String::fromUTF8 (processor_.parameters().spec (index).name.c_str());
}

juce::String RouterView::conditionSummary (const rt::Rule& r) const
{
    if (r.terms.empty()) return text ("router.cond_always");
    const char* ops[rt::numCombines] = { "router.op_and", "router.op_or", "router.op_xor", "router.op_nand", "router.op_nor" };
    juce::StringArray names;
    for (const auto& t : r.terms) names.add ((t.negate ? text ("router.not") + " " : juce::String()) + paramName (t.param));
    return text (ops[static_cast<int>(r.combine)]) + " : " + names.joinIntoString (", ");
}

// Menu de la condition d'une règle : sans condition, opérateur, tirants à ajouter ou retirer ou inverser.
void RouterView::editCondition (std::size_t index, juce::Component& anchor)
{
    if (index >= config_.rules.size()) return;
    const auto& rule = config_.rules[index];
    juce::PopupMenu menu;
    menu.addItem (1, text ("router.cond_always"), true, rule.terms.empty());
    menu.addSeparator();
    const char* ops[rt::numCombines] = { "router.op_and", "router.op_or", "router.op_xor", "router.op_nand", "router.op_nor" };
    juce::PopupMenu opMenu;
    for (int i = 0; i < rt::numCombines; ++i) opMenu.addItem (10 + i, text (ops[i]), true, static_cast<int> (rule.combine) == i);
    menu.addSubMenu (text ("router.cond_operator"), opMenu, ! rule.terms.empty());
    menu.addSeparator();
    juce::PopupMenu add;
    for (int i = 0; i < rt::kSwitches; ++i) add.addItem (100 + i, paramName (rt::switchParam (i)));
    add.addSeparator();
    add.addItem (2, text ("router.cond_other"));
    menu.addSubMenu (text ("router.cond_add"), add, rule.terms.size() < static_cast<std::size_t> (rt::kMaxTerms));
    for (std::size_t k = 0; k < rule.terms.size(); ++k)
    {
        menu.addItem (200 + static_cast<int> (k), text ("router.cond_remove") + " : " + paramName (rule.terms[k].param));
        menu.addItem (300 + static_cast<int> (k), text ("router.cond_invert") + " : " + paramName (rule.terms[k].param), true, rule.terms[k].negate);
    }
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [safe = juce::Component::SafePointer<RouterView> (this), index] (int choice)
    {
        if (safe == nullptr || choice == 0 || index >= safe->config_.rules.size()) return;
        auto& r = safe->config_.rules[index];
        if (choice == 1) r.terms.clear();
        else if (choice == 2) { safe->addOtherTerm (index); return; }
        else if (choice >= 10 && choice < 10 + rt::numCombines) r.combine = static_cast<rt::Combine> (choice - 10);
        else if (choice >= 100 && choice < 100 + rt::kSwitches) r.terms.push_back ({ rt::switchParam (choice - 100), false });
        else if (choice >= 200 && choice < 300 && static_cast<std::size_t> (choice - 200) < r.terms.size()) r.terms.erase (r.terms.begin() + (choice - 200));
        else if (choice >= 300 && choice < 400 && static_cast<std::size_t> (choice - 300) < r.terms.size()) r.terms[static_cast<std::size_t> (choice - 300)].negate = ! r.terms[static_cast<std::size_t> (choice - 300)].negate;
        else return;
        safe->commit();
        safe->rebuildRows();
    });
}

// Un autre paramètre comme condition : piston, interrupteur d'orgue, mute... (vrai à partir de la moitié de sa course)
void RouterView::addOtherTerm (std::size_t index)
{
    if (search_ != nullptr || index >= config_.rules.size()) return;
    search_ = std::make_unique<ParamSearchPanel> (processor_.parameters(), palette_, tr_, processor_.selectedTrack(), false,
        [safe = juce::Component::SafePointer<RouterView> (this), index] (neptune::core::ParamId id)
        {
            if (safe == nullptr || index >= safe->config_.rules.size() || safe->config_.rules[index].terms.size() >= static_cast<std::size_t> (rt::kMaxTerms)) return;
            safe->config_.rules[index].terms.push_back ({ id, false });
            safe->commit();
            safe->rebuildRows();
        },
        [safe = juce::Component::SafePointer<RouterView> (this)]
        {
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->search_.reset(); });
        });
    addAndMakeVisible (*search_);
    search_->setBounds (getLocalBounds());
    search_->toFront (true);
}
