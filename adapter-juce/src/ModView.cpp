#include "ModView.h"


#include <algorithm>

#include "neptune/core/Control.h"

using neptune::core::theme::Token;
namespace mod = neptune::core::modulation;

namespace
{
// Largeurs des colonnes d'une route (la ligne de légende au-dessus de la liste reprend les mêmes).
constexpr int kRemoveW = 34, kDestW = 300, kKindW = 176, kIndexW = 90, kChannelW = 90, kRangeW = 84, kGapS = 6, kGapL = 10;
}

// ------------------------------------------------------------------------------------------------ une route

class ModView::RouteRow : public juce::Component
{
public:
    RouteRow (ModView& owner, std::size_t index) : owner_ (owner), index_ (index)
    {
        const auto& r = owner.matrix_.routes[index];
        const char* kinds[7] = { "mod.src_cc", "mod.src_bend", "mod.src_aftertouch", "mod.src_macro", "mod.src_lfo", "mod.src_env", "mod.src_wheel" };
        for (const int id : { 1, 7, 2, 3, 4, 5, 6 }) kind_.addItem (owner.text (kinds[id - 1]), id);   // « Molette de modulation » (id 7) juste après les CC
        kind_.onChange = [this] { kindChanged(); };
        addAndMakeVisible (kind_);
        indexBox_.onChange = [this] { if (! updating_) { edit().source.index = indexBox_.getSelectedId() - 1; owner_.commit(); } };
        addAndMakeVisible (indexBox_);
        channel_.addItem (owner.text ("mod.any_channel"), 1);
        for (int c = 1; c <= 16; ++c) channel_.addItem (juce::String (c), c + 1);
        channel_.onChange = [this] { if (! updating_) { edit().source.channel = channel_.getSelectedId() - 1; owner_.commit(); } };
        addAndMakeVisible (channel_);
        amount_.setPalette (owner.palette_);
        {
            neptune::core::dragvalue::Range range;
            range.min = -100.0; range.max = 100.0; range.step = 1.0; range.decimals = 0; range.def = 0.0;
            amount_.setRange (range, "%");
        }
        amount_.setTooltip (owner.text ("mod.tip_amount"));
        amount_.onGestureBegin = [this] { owner_.host_.modBeginGesture(); };
        amount_.onGestureEnd = [this] { owner_.host_.modEndGesture(); };
        amount_.onValue = [this] (double v) { if (! updating_) { edit().amount = v / 100.0; owner_.commit(); } };
        addAndMakeVisible (amount_);
        centred_.setTooltip (owner.text ("mod.tip_centred"));
        centred_.setClickingTogglesState (true);
        centred_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::Accent));
        centred_.onClick = [this] { if (! updating_) { edit().centred = centred_.getToggleState(); refresh (edit()); owner_.commit(); } };
        addAndMakeVisible (centred_);
        dest_.onClick = [this] { owner_.chooseDestination (index_); };
        addAndMakeVisible (dest_);
        remove_.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        remove_.setTooltip (owner.text ("mod.remove"));
        remove_.onClick = [this] { owner_.removeRoute (index_); };
        addAndMakeVisible (remove_);
        refresh (r);
    }

    void paint (juce::Graphics& g) override   // une carte par route
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
        remove_.setBounds (r.removeFromRight (kRemoveW));
        r.removeFromRight (kGapS);
        dest_.setBounds (r.removeFromRight (kDestW));
        r.removeFromRight (kGapS);
        kind_.setBounds (r.removeFromLeft (kKindW));
        r.removeFromLeft (kGapS);
        indexBox_.setBounds (r.removeFromLeft (kIndexW));
        r.removeFromLeft (kGapS);
        channel_.setBounds (r.removeFromLeft (kChannelW));
        r.removeFromLeft (kGapL);
        centred_.setBounds (r.removeFromRight (kRangeW));
        amount_.setBounds (r);
    }

    void refresh (const mod::Route& r)
    {
        updating_ = true;
        using SK = mod::SourceKind;
        int kindId = 5;   // LFO par défaut
        switch (r.source.kind)
        {
            case SK::ControlChange: kindId = r.source.index == 1 ? 7 : 1; break;   // CC 1 = la molette de modulation
            case SK::PitchBend: kindId = 2; break;
            case SK::Aftertouch: kindId = 3; break;
            case SK::Macro: kindId = 4; break;
            case SK::Lfo: kindId = 5; break;
            case SK::Envelope: kindId = 6; break;
            default: break;
        }
        kind_.setSelectedId (kindId, juce::dontSendNotification);
        fillIndex (r.source.kind);
        indexBox_.setSelectedId (r.source.index + 1, juce::dontSendNotification);
        channel_.setSelectedId (r.source.channel + 1, juce::dontSendNotification);
        amount_.setValue (r.amount * 100.0);
        // plage de la source : 0..1 (n'ajoute que vers le haut) ou -1..+1 (centrée) ; LFO et pitch bend sont toujours -1..+1
        const bool bipolar = mod::isBipolar (r.source.kind);
        centred_.setToggleState (bipolar || r.centred, juce::dontSendNotification);
        centred_.setButtonText (owner_.text (bipolar || r.centred ? "mod.range_bipolar" : "mod.range_unipolar"));
        centred_.setEnabled (! bipolar);
        destName_ = r.dest != 0 ? owner_.host_.modDestName (owner_.track_, r.dest) : std::string();
        showDest();
        updating_ = false;
    }

    // Avertit quand la modulation ne peut pas aller dans le sens voulu : source unipolaire, quantité dans un sens, et le réglage déjà au bout de sa course
    // (une coupure ouverte au maximum ne peut pas monter). Appelée à chaque tick de la vue.
    void updateLimit()
    {
        bool limited = false;
        const auto& r = owner_.matrix_.routes[index_];
        if (r.dest != 0 && r.amount != 0.0 && ! mod::isBipolar (r.source.kind) && ! r.centred)
        {
            const auto& set = owner_.host_.modParameters();
            if (const auto i = set.indexOf (r.dest); i != neptune::core::ParameterSet::npos)
            {
                const double n = set.getNormalizedAt (i);
                limited = r.amount > 0.0 ? n >= 0.999 : n <= 0.001;
            }
        }
        if (limited == atLimit_) return;
        atLimit_ = limited;
        showDest();
    }

private:
    mod::Route& edit() { return owner_.matrix_.routes[index_]; }

    void showDest()
    {
        auto label = ! destName_.empty() ? juce::String::fromUTF8 (destName_.c_str()) : owner_.text ("mod.choose_dest");
        if (atLimit_) label = "! " + label;
        dest_.setButtonText (label);
        dest_.setTooltip (atLimit_ ? owner_.text ("mod.at_limit") : juce::String());
        dest_.setColour (juce::TextButton::textColourOffId, atLimit_ ? juce::Colour (0xffe8a33d) : owner_.colour (Token::Text));
    }

    void fillIndex (mod::SourceKind kind)
    {
        indexBox_.clear (juce::dontSendNotification);
        using SK = mod::SourceKind;
        const bool hasIndex = kind == SK::ControlChange || kind == SK::Macro || kind == SK::Lfo || kind == SK::Envelope;
        const int count = kind == SK::ControlChange ? 128 : kind == SK::Macro ? neptune::core::control::kMacros : kind == SK::Lfo ? mod::kLfos : kind == SK::Envelope ? mod::kEnvelopes : 0;
        for (int i = 0; i < count; ++i) indexBox_.addItem (juce::String (kind == SK::ControlChange ? i : i + 1), i + 1);   // les CC se comptent à partir de 0
        indexBox_.setVisible (hasIndex);
        channel_.setVisible (kind == SK::ControlChange || kind == SK::PitchBend || kind == SK::Aftertouch);
    }

    void kindChanged()
    {
        if (updating_) return;
        using SK = mod::SourceKind;
        const SK kinds[6] = { SK::ControlChange, SK::PitchBend, SK::Aftertouch, SK::Macro, SK::Lfo, SK::Envelope };
        auto& r = edit();
        const int chosen = kind_.getSelectedId();
        if (chosen == 7) { r.source = { SK::ControlChange, 1, 0 }; refresh (r); owner_.commit(); return; }   // molette de modulation : CC 1, tous canaux
        r.source.kind = kinds[std::clamp (chosen - 1, 0, 5)];
        r.source.index = 0;
        if (r.source.kind != SK::ControlChange && r.source.kind != SK::PitchBend && r.source.kind != SK::Aftertouch) r.source.channel = 0;
        refresh (r);
        owner_.commit();
    }

    ModView& owner_;
    std::size_t index_;
    std::string destName_;
    bool atLimit_ = false;
    bool updating_ = false;
    juce::ComboBox kind_, indexBox_, channel_;
    DragValuePill amount_;
    juce::TextButton centred_;
    juce::TextButton dest_, remove_;
};

// ------------------------------------------------------------------------------------------------ la vue

ModView::ModView (ModulationHost& host, const neptune::core::theme::Palette& palette, const neptune::core::i18n::Translator& tr)
    : host_ (host), palette_ (palette), tr_ (tr)
{
    auto caption = [this] (juce::Label& l, const char* key)
    {
        l.setText (text (key), juce::dontSendNotification);
        l.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, colour (Token::Accent));
        addAndMakeVisible (l);
    };
    caption (lfoCaption_, "mod.lfos");
    caption (envCaption_, "mod.envelopes");
    caption (routesCaption_, "mod.routes");
    hint_.setText (text ("mod.matrix_hint"), juce::dontSendNotification);
    hint_.setFont (juce::FontOptions (13.0f));
    hint_.setColour (juce::Label::textColourId, colour (Token::TextDim));
    addAndMakeVisible (hint_);
    auto legend = [this] (juce::Label& l, const char* key)
    {
        l.setText (text (key), juce::dontSendNotification);
        l.setFont (juce::FontOptions (13.0f));
        l.setColour (juce::Label::textColourId, colour (Token::TextDim));
        l.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (l);
    };
    legend (lfoHead_[0], "mod.head_shape");
    legend (lfoHead_[1], "mod.head_rate");
    legend (lfoHead_[2], "mod.head_phase");
    const char* routeKeys[6] = { "mod.head_source", "mod.head_number", "mod.head_channel", "mod.head_amount", "mod.head_range", "mod.head_dest" };
    for (int i = 0; i < 6; ++i) legend (routeHead_[static_cast<std::size_t> (i)], routeKeys[i]);

    const char* shapes[mod::numLfoShapes] = { "mod.shape_sine", "mod.shape_triangle", "mod.shape_saw_up", "mod.shape_saw_down", "mod.shape_square", "mod.shape_sh" };
    for (int i = 0; i < mod::kLfos; ++i)
    {
        auto& row = lfoRows_[static_cast<std::size_t> (i)];
        row.label.setText ("LFO " + juce::String (i + 1), juce::dontSendNotification);
        row.label.setColour (juce::Label::textColourId, colour (Token::Text));
        addAndMakeVisible (row.label);
        for (int s = 0; s < mod::numLfoShapes; ++s) row.shape.addItem (text (shapes[s]), s + 1);
        row.shape.onChange = [this, i] { if (! updating_) { matrix_.lfos[static_cast<std::size_t> (i)].shape = static_cast<mod::LfoShape> (lfoRows_[static_cast<std::size_t> (i)].shape.getSelectedId() - 1); commit(); } };
        addAndMakeVisible (row.shape);
        for (auto* pill : { &row.rate, &row.phase })
        {
            pill->setPalette (palette_);
            pill->onGestureBegin = [this] { host_.modBeginGesture(); };
            pill->onGestureEnd = [this] { host_.modEndGesture(); };
            addAndMakeVisible (*pill);
        }
        {
            neptune::core::dragvalue::Range rateRange, phaseRange;
            rateRange.min = 0.01; rateRange.max = 50.0; rateRange.log = true; rateRange.decimals = 2; rateRange.def = 1.0;   // glissé régulier sur toute l'étendue
            phaseRange.min = 0.0; phaseRange.max = 100.0; phaseRange.step = 1.0; phaseRange.decimals = 0;
            row.rate.setRange (rateRange, "Hz");
            row.phase.setRange (phaseRange, "%");
        }
        row.rate.setTooltip (text ("mod.tip_rate"));
        row.phase.setTooltip (text ("mod.tip_phase"));
        row.rate.onValue = [this, i] (double v) { if (! updating_) { matrix_.lfos[static_cast<std::size_t> (i)].rateHz = v; commit(); } };
        row.phase.onValue = [this, i] (double v) { if (! updating_) { matrix_.lfos[static_cast<std::size_t> (i)].phase = v / 100.0; commit(); } };
    }
    envEditor_ = std::make_unique<EnvelopeEditor> (palette_, tr_);
    envEditor_->onGestureBegin = [this] { host_.modBeginGesture(); };
    envEditor_->onGestureEnd = [this] { host_.modEndGesture(); };
    envEditor_->onChange = [this] (const neptune::core::EnvelopeDescription& e)
    {
        if (updating_) return;
        matrix_.envelopes[static_cast<std::size_t> (envSel_)].env = e;
        commit();
    };
    addAndMakeVisible (*envEditor_);
    for (int i = 0; i < mod::kEnvelopes; ++i)
    {
        auto& b = envButtons_[static_cast<std::size_t> (i)];
        b.setButtonText ("ENV " + juce::String (i + 1));
        b.setClickingTogglesState (true);
        b.setRadioGroupId (4812);
        b.setColour (juce::TextButton::buttonOnColourId, colour (Token::Accent));
        b.onClick = [this, i] { if (envButtons_[static_cast<std::size_t> (i)].getToggleState()) selectEnvelope (i); };
        addAndMakeVisible (b);
    }
    envButtons_[0].setToggleState (true, juce::dontSendNotification);
    envTag_.setText ("ENV 1", juce::dontSendNotification);
    envTag_.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    envTag_.setColour (juce::Label::textColourId, colour (Token::Accent));
    envTag_.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (envTag_);
    addButton_.setButtonText (text ("mod.add_route"));
    addButton_.onClick = [this] { addRoute(); };
    addAndMakeVisible (addButton_);
    addAndMakeVisible (view_);
    view_.setViewedComponent (&routesHolder_, false);
    view_.setScrollBarsShown (true, false);
    reload();
    startTimerHz (8);
}

ModView::~ModView() { stopTimer(); }

juce::Colour ModView::colour (Token t) const
{
    const auto& c = palette_.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

void ModView::paint (juce::Graphics& g)
{
    for (const auto& r : { lfoPanel_, envPanel_, routesPanel_ })
    {
        if (r.isEmpty()) continue;
        const auto b = r.toFloat().reduced (0.5f);
        g.setColour (colour (Token::PanelBg));
        g.fillRoundedRectangle (b, 10.0f);
        g.setColour (colour (Token::Pb));
        g.drawRoundedRectangle (b, 10.0f, 1.0f);
    }
}

void ModView::resized()
{
    // trois conteneurs arrondis : LFO (en haut à gauche), enveloppes (en haut à droite), routes (en bas)
    auto r = getLocalBounds();
    auto topRow = r.removeFromTop (300);
    lfoPanel_ = topRow.removeFromLeft (topRow.getWidth() / 2 - 6);
    topRow.removeFromLeft (12);
    envPanel_ = topRow;
    r.removeFromTop (12);
    routesPanel_ = r;
    auto left = lfoPanel_.reduced (14, 10);
    auto top = envPanel_.reduced (14, 10);
    r = routesPanel_.reduced (14, 10);
    lfoCaption_.setBounds (left.removeFromTop (26));
    envCaption_.setBounds (top.removeFromTop (26));
    {
        auto head = left.removeFromTop (20);
        head.removeFromLeft (60);
        lfoHead_[0].setBounds (head.removeFromLeft (150));
        head.removeFromLeft (8);
        lfoHead_[1].setBounds (head.removeFromLeft (96));
        head.removeFromLeft (8);
        lfoHead_[2].setBounds (head.removeFromLeft (80));
    }
    for (int i = 0; i < mod::kLfos; ++i)
    {
        auto row = left.removeFromTop (34);
        auto& l = lfoRows_[static_cast<std::size_t> (i)];
        l.label.setBounds (row.removeFromLeft (60));
        l.shape.setBounds (row.removeFromLeft (150).reduced (0, 2));
        row.removeFromLeft (8);
        l.rate.setBounds (row.removeFromLeft (96).reduced (0, 2));
        row.removeFromLeft (8);
        l.phase.setBounds (row.removeFromLeft (80).reduced (0, 2));
    }
    auto selector = top.removeFromTop (30);
    for (auto& b : envButtons_) { b.setBounds (selector.removeFromLeft (80).reduced (2)); }
    envTag_.setBounds (selector);
    envEditor_->setBounds (top);
    auto head = r.removeFromTop (30);
    routesCaption_.setBounds (head.removeFromLeft (200));
    addButton_.setBounds (head.removeFromRight (160));
    hint_.setBounds (head);
    r.removeFromTop (4);
    auto legendRow = r.removeFromTop (20);
    view_.setBounds (r);
    layoutRoutes();
    {
        // même géométrie que RouteRow::resized(), sur la largeur visible de la liste
        const int width = std::max (400, view_.getMaximumVisibleWidth());
        auto row = legendRow.withWidth (width).reduced (4, 0);
        row.removeFromRight (kRemoveW + kGapS);
        routeHead_[5].setBounds (row.removeFromRight (kDestW));
        row.removeFromRight (kGapS);
        routeHead_[4].setBounds (row.removeFromRight (kRangeW));
        routeHead_[0].setBounds (row.removeFromLeft (kKindW));
        row.removeFromLeft (kGapS);
        routeHead_[1].setBounds (row.removeFromLeft (kIndexW));
        row.removeFromLeft (kGapS);
        routeHead_[2].setBounds (row.removeFromLeft (kChannelW));
        row.removeFromLeft (kGapL);
        routeHead_[3].setBounds (row);
    }
    if (search_ != nullptr) search_->setBounds (getLocalBounds());
}

void ModView::layoutRoutes()
{
    const int width = std::max (400, view_.getMaximumVisibleWidth());
    int y = 0;
    for (auto& row : rows_) { row->setBounds (0, y, width, 36); y += 38; }
    routesHolder_.setSize (width, std::max (y, 1));
}

void ModView::reload()
{
    track_ = host_.modTrack();
    committed_ = host_.modMatrix (track_);
    matrix_ = committed_;
    updating_ = true;
    for (int i = 0; i < mod::kLfos; ++i)
    {
        const auto& d = matrix_.lfos[static_cast<std::size_t> (i)];
        auto& row = lfoRows_[static_cast<std::size_t> (i)];
        row.shape.setSelectedId (static_cast<int> (d.shape) + 1, juce::dontSendNotification);
        row.rate.setValue (d.rateHz);
        row.phase.setValue (d.phase * 100.0);
    }
    envEditor_->setEnvelope (matrix_.envelopes[static_cast<std::size_t> (envSel_)].env);
    updating_ = false;
    rebuildRoutes();
}

void ModView::rebuildRoutes()
{
    rows_.clear();
    for (std::size_t i = 0; i < matrix_.routes.size(); ++i)
    {
        rows_.push_back (std::make_unique<RouteRow> (*this, i));
        routesHolder_.addAndMakeVisible (*rows_.back());
    }
    addButton_.setEnabled (matrix_.routes.size() < mod::kMaxRoutes);
    layoutRoutes();
}

void ModView::selectEnvelope (int index)
{
    envSel_ = std::clamp (index, 0, mod::kEnvelopes - 1);
    envTag_.setText ("ENV " + juce::String (envSel_ + 1), juce::dontSendNotification);
    updating_ = true;
    envEditor_->setEnvelope (matrix_.envelopes[static_cast<std::size_t> (envSel_)].env);
    updating_ = false;
}

void ModView::commit()
{
    committed_ = mod::sanitised (matrix_);
    host_.modEdit (track_, committed_);
}

void ModView::timerCallback()
{
    if (! isShowing()) return;
    if (host_.modTrack() != track_) { reload(); return; }
    if (host_.modMatrix (track_) != committed_) reload();   // annulation, chargement... : la matrice a changé ailleurs
    for (auto& row : rows_) row->updateLimit();
}

void ModView::addRoute()
{
    if (matrix_.routes.size() >= mod::kMaxRoutes) return;
    mod::Route r;
    r.source = { mod::SourceKind::Lfo, 0, 0 };
    matrix_.routes.push_back (r);
    rebuildRoutes();
    chooseDestination (matrix_.routes.size() - 1);
}

void ModView::removeRoute (std::size_t index)
{
    if (index >= matrix_.routes.size()) return;
    matrix_.routes.erase (matrix_.routes.begin() + static_cast<std::ptrdiff_t> (index));
    commit();
    rebuildRoutes();
}

void ModView::chooseDestination (std::size_t index)
{
    if (search_ != nullptr || index >= matrix_.routes.size()) return;
    search_ = std::make_unique<ParamSearchPanel> (host_.modParameters(), palette_, tr_, track_, true,
        [safe = juce::Component::SafePointer<ModView> (this), index] (neptune::core::ParamId id)
        {
            if (safe == nullptr || index >= safe->matrix_.routes.size()) return;
            safe->matrix_.routes[index].dest = id;
            safe->commit();
            safe->rebuildRoutes();
        },
        [safe = juce::Component::SafePointer<ModView> (this)]
        {
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->search_.reset(); });
        },
        host_.modInsertTargets (track_), host_.modShowScope());
    addAndMakeVisible (*search_);
    search_->setBounds (getLocalBounds());
    search_->toFront (true);
}
