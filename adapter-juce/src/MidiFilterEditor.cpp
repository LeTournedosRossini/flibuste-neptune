#include "MidiFilterEditor.h"

#include <algorithm>

using namespace neptune::core;
using neptune::core::theme::Token;

namespace
{
juce::String U (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
using Path = std::vector<int>;

midi::Rule* ruleAt (midi::Rule& root, const Path& path)
{
    midi::Rule* r = &root;
    for (const int i : path)
    {
        if (i < 0 || i >= static_cast<int> (r->children.size())) return nullptr;
        r = &r->children[static_cast<std::size_t> (i)];
    }
    return r;
}

const midi::Rule* ruleAtConst (const midi::Rule& root, const Path& path)
{
    const midi::Rule* r = &root;
    for (const int i : path)
    {
        if (i < 0 || i >= static_cast<int> (r->children.size())) return nullptr;
        r = &r->children[static_cast<std::size_t> (i)];
    }
    return r;
}

const char* const kindKeys[midi::numMsgKinds] = { "midi.kind_note", "midi.kind_note_on", "midi.kind_note_off", "midi.kind_cc",
                                                  "midi.kind_pitch_bend", "midi.kind_channel_pressure", "midi.kind_poly_pressure",
                                                  "midi.kind_program_change" };
const char* const opKeys[midi::numOps] = { "", "midi.op_and", "midi.op_or", "midi.op_xor", "midi.op_nand", "midi.op_nor", "midi.op_not" };
} // namespace

struct MidiFilterEditor::Impl : juce::ListBoxModel
{
    struct Row { Path path; int depth = 0; };

    Impl (MidiFilterEditor& o, const theme::Palette& p, const i18n::Translator& t, int trackIndex,
          std::function<midi::FilterDescription()> r, std::function<void (const midi::FilterDescription&)> w, std::function<void()> c)
        : owner (o), palette (p), tr (t), track (trackIndex), read (std::move (r)), write (std::move (w)), close (std::move (c))
    {
        d = read();
        title.setText (text ("midi.title") + " " + juce::String (track + 1), juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, colour (Token::Text));
        owner.addAndMakeVisible (title);
        hint.setText (text ("midi.hint"), juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, colour (Token::TextDim));
        hint.setFont (juce::FontOptions (11.5f));
        owner.addAndMakeVisible (hint);
        preview.setColour (juce::Label::textColourId, colour (Token::Accent));
        preview.setFont (juce::FontOptions (12.0f));
        preview.setMinimumHorizontalScale (0.7f);
        owner.addAndMakeVisible (preview);

        enabled.setButtonText (text ("midi.enabled"));
        enabled.setColour (juce::ToggleButton::textColourId, colour (Token::Text));
        enabled.onClick = [this] { if (! updating) { d.enabled = enabled.getToggleState(); commit(); } };
        owner.addAndMakeVisible (enabled);

        list.setModel (this);
        list.setColour (juce::ListBox::backgroundColourId, colour (Token::PanelBg));
        list.setColour (juce::ListBox::outlineColourId, colour (Token::CtlBd));
        list.setOutlineThickness (1);
        list.setRowHeight (26);
        owner.addAndMakeVisible (list);

        auto button = [this] (juce::TextButton& b, const char* key, std::function<void()> fn)
        {
            b.setButtonText (text (key));
            b.onClick = std::move (fn);
            owner.addAndMakeVisible (b);
        };
        button (addLeaf, "midi.add_rule", [this] { addRule (false); });
        button (addGroup, "midi.add_group", [this] { addRule (true); });
        button (remove, "midi.delete", [this] { removeSelected(); });
        button (up, "midi.up", [this] { moveSelected (-1); });
        button (down, "midi.down", [this] { moveSelected (+1); });
        button (closeButton, "modal.close", [this] { if (close) close(); });

        auto label = [this] (juce::Label& l, const char* key)
        {
            l.setText (text (key), juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, colour (Token::TextDim));
            l.setFont (juce::FontOptions (12.0f));
            owner.addAndMakeVisible (l);
        };
        label (opLabel, "midi.op");
        label (kindLabel, "midi.kind");
        label (channelLabel, "midi.channel");
        label (range1Label, "midi.range_note");
        label (range2Label, "midi.range_velocity");

        auto combo = [this] (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, colour (Token::BgCard));
            c.setColour (juce::ComboBox::textColourId, colour (Token::Text));
            c.setColour (juce::ComboBox::outlineColourId, colour (Token::CtlBd));
            owner.addAndMakeVisible (c);
        };
        for (int i = 1; i < midi::numOps; ++i) opBox.addItem (text (opKeys[i]), i);
        for (int i = 0; i < midi::numMsgKinds; ++i) kindBox.addItem (text (kindKeys[i]), i + 1);
        channelBox.addItem (text ("midi.channel_any"), 1);
        for (int c = 1; c <= 16; ++c) channelBox.addItem (juce::String (c), c + 1);
        for (auto* c : { &opBox, &kindBox, &channelBox }) combo (*c);
        opBox.onChange = [this]
        {
            if (updating) return;
            if (auto* r = selectedRule()) { r->op = static_cast<midi::Op> (opBox.getSelectedId()); commit(); }
        };
        kindBox.onChange = [this]
        {
            if (updating) return;
            if (auto* r = selectedRule()) { r->condition.kind = static_cast<midi::MsgKind> (kindBox.getSelectedId() - 1); commit(); }
        };
        channelBox.onChange = [this]
        {
            if (updating) return;
            if (auto* r = selectedRule()) { r->condition.channel = channelBox.getSelectedId() - 1; commit(); }
        };
        for (auto* s : { &lo1, &hi1, &lo2, &hi2 })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 22);
            s->setRange (0, 127, 1);
            s->setColour (juce::Slider::trackColourId, colour (Token::Accent));
            s->setColour (juce::Slider::backgroundColourId, colour (Token::Grid));
            s->setColour (juce::Slider::thumbColourId, colour (Token::Handle));
            s->setColour (juce::Slider::textBoxTextColourId, colour (Token::Text));
            s->setColour (juce::Slider::textBoxOutlineColourId, colour (Token::CtlBd));
            s->onValueChange = [this] { if (! updating) applyRanges(); };
            owner.addAndMakeVisible (*s);
        }
        list.selectRow (0);
        refresh();
    }

    juce::String text (const char* key) const { return U (std::string (tr.tr (key))); }
    juce::Colour colour (Token t) const
    {
        const auto& c = palette.color (t);
        return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
    }

    // ---- données
    void flatten (const midi::Rule& r, Path path, int depth)
    {
        rows.push_back ({ path, depth });
        for (std::size_t i = 0; i < r.children.size(); ++i)
        {
            auto p = path;
            p.push_back (static_cast<int> (i));
            flatten (r.children[i], p, depth + 1);
        }
    }

    void rebuildRows()
    {
        rows.clear();
        flatten (d.root, {}, 0);
        list.updateContent();
    }

    midi::Rule* selectedRule() { return ruleAt (d.root, selected); }

    void commit()
    {
        d = midi::sanitised (d);
        write (d);
        refresh();
    }

    void refresh()
    {
        updating = true;
        rebuildRows();
        enabled.setToggleState (d.enabled, juce::dontSendNotification);
        int sel = 0;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].path == selected) sel = static_cast<int> (i);
        if (rows.empty()) sel = -1;
        if (sel >= 0) { list.selectRow (sel, true, false); selected = rows[static_cast<std::size_t> (sel)].path; }
        const auto* r = selectedRule();
        const bool leaf = r != nullptr && r->op == midi::Op::Leaf;
        const bool group = r != nullptr && r->op != midi::Op::Leaf;
        for (auto* c : std::initializer_list<juce::Component*> { &opLabel, &opBox }) c->setVisible (group);
        for (auto* c : std::initializer_list<juce::Component*> { &kindLabel, &kindBox, &channelLabel, &channelBox, &range1Label, &lo1, &hi1,
                                                                 &range2Label, &lo2, &hi2 })
            c->setVisible (leaf);
        if (group) opBox.setSelectedId (static_cast<int> (r->op), juce::dontSendNotification);
        if (leaf)
        {
            const auto& c = r->condition;
            kindBox.setSelectedId (static_cast<int> (c.kind) + 1, juce::dontSendNotification);
            channelBox.setSelectedId (c.channel + 1, juce::dontSendNotification);
            lo1.setValue (c.lo1, juce::dontSendNotification);
            hi1.setValue (c.hi1, juce::dontSendNotification);
            lo2.setValue (c.lo2, juce::dontSendNotification);
            hi2.setValue (c.hi2, juce::dontSendNotification);
            const auto k = c.kind;
            const bool uses1 = k != midi::MsgKind::PitchBend && k != midi::MsgKind::ChannelPressure;
            const bool uses2 = k != midi::MsgKind::ProgramChange;
            range1Label.setText (text (k == midi::MsgKind::ControlChange ? "midi.range_cc"
                                       : k == midi::MsgKind::ProgramChange ? "midi.range_program" : "midi.range_note"), juce::dontSendNotification);
            range2Label.setText (text (k == midi::MsgKind::ControlChange ? "midi.range_value"
                                       : k == midi::MsgKind::PitchBend ? "midi.range_bend"
                                       : (k == midi::MsgKind::ChannelPressure || k == midi::MsgKind::PolyPressure) ? "midi.range_pressure"
                                                                                                                   : "midi.range_velocity"),
                                 juce::dontSendNotification);
            for (auto* c2 : std::initializer_list<juce::Component*> { &range1Label, &lo1, &hi1 }) c2->setVisible (uses1);
            for (auto* c2 : std::initializer_list<juce::Component*> { &range2Label, &lo2, &hi2 }) c2->setVisible (uses2);
        }
        remove.setEnabled (! selected.empty());
        up.setEnabled (! selected.empty());
        down.setEnabled (! selected.empty());
        preview.setText (juce::String (d.enabled ? "" : "(" + std::string (tr.tr ("midi.off")) + ") ") + U (midi::describe (d.root)), juce::dontSendNotification);
        updating = false;
        list.repaint();
    }

    void addRule (bool asGroup)
    {
        auto* target = selectedRule();
        if (target == nullptr) target = &d.root;
        Path parentPath = selected;
        if (target->op == midi::Op::Leaf)
        {
            if (parentPath.empty()) return;
            parentPath.pop_back();
            target = ruleAt (d.root, parentPath);
            if (target == nullptr) return;
        }
        if (target->op == midi::Op::Not && ! target->children.empty()) return; // NON : un seul enfant
        midi::Rule r;
        r.op = asGroup ? midi::Op::And : midi::Op::Leaf;
        r.condition.kind = midi::MsgKind::Note;
        if (midi::countNodes (d.root) >= midi::maxNodes || target->children.size() >= static_cast<std::size_t> (midi::maxChildren)) return;
        target->children.push_back (std::move (r));
        selected = parentPath;
        selected.push_back (static_cast<int> (target->children.size()) - 1);
        d.enabled = true;
        commit();
    }

    void removeSelected()
    {
        if (selected.empty()) return;
        Path parentPath = selected;
        const int index = parentPath.back();
        parentPath.pop_back();
        if (auto* parent = ruleAt (d.root, parentPath); parent != nullptr && index < static_cast<int> (parent->children.size()))
        {
            parent->children.erase (parent->children.begin() + index);
            selected = parentPath;
            commit();
        }
    }

    void moveSelected (int delta)
    {
        if (selected.empty()) return;
        Path parentPath = selected;
        const int index = parentPath.back();
        parentPath.pop_back();
        auto* parent = ruleAt (d.root, parentPath);
        if (parent == nullptr) return;
        const int target = index + delta;
        if (target < 0 || target >= static_cast<int> (parent->children.size())) return;
        std::swap (parent->children[static_cast<std::size_t> (index)], parent->children[static_cast<std::size_t> (target)]);
        selected.back() = target;
        commit();
    }

    void applyRanges()
    {
        if (auto* r = selectedRule(); r != nullptr && r->op == midi::Op::Leaf)
        {
            r->condition.lo1 = static_cast<int> (lo1.getValue());
            r->condition.hi1 = static_cast<int> (hi1.getValue());
            r->condition.lo2 = static_cast<int> (lo2.getValue());
            r->condition.hi2 = static_cast<int> (hi2.getValue());
            commit();
        }
    }

    // ---- ListBoxModel
    int getNumRows() override { return static_cast<int> (rows.size()); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (row < 0 || row >= static_cast<int> (rows.size())) return;
        const auto& rw = rows[static_cast<std::size_t> (row)];
        if (sel) { g.setColour (colour (Token::Accent).withAlpha (0.28f)); g.fillRect (0, 0, w, h); }
        g.setColour (colour (Token::Border).withAlpha (0.4f));
        g.drawHorizontalLine (h - 1, 0.0f, static_cast<float> (w));
        const auto* r = ruleAtConst (d.root, rw.path);
        if (r == nullptr) return;
        juce::String label;
        if (r->op == midi::Op::Leaf) label = text (kindKeys[static_cast<int> (r->condition.kind)]) + "  " + U (midi::describe (*r));
        else label = text (opKeys[static_cast<int> (r->op)]) + (rw.path.empty() ? "  (" + text ("midi.root") + ")" : juce::String());
        g.setColour (colour (r->op == midi::Op::Leaf ? Token::Text : Token::Accent));
        g.setFont (juce::FontOptions (13.0f, r->op == midi::Op::Leaf ? juce::Font::plain : juce::Font::bold));
        g.drawText (label, 8 + rw.depth * 18, 0, w - 12 - rw.depth * 18, h, juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged (int last) override
    {
        if (updating || last < 0 || last >= static_cast<int> (rows.size())) return;
        selected = rows[static_cast<std::size_t> (last)].path;
        refresh();
    }

    MidiFilterEditor& owner;
    theme::Palette palette;
    const i18n::Translator& tr;
    int track;
    std::function<midi::FilterDescription()> read;
    std::function<void (const midi::FilterDescription&)> write;
    std::function<void()> close;

    midi::FilterDescription d;
    Path selected;
    std::vector<Row> rows;
    bool updating = false;

    juce::Label title, hint, preview, opLabel, kindLabel, channelLabel, range1Label, range2Label;
    juce::ToggleButton enabled;
    juce::ListBox list { "rules", nullptr };
    juce::TextButton addLeaf, addGroup, remove, up, down, closeButton;
    juce::ComboBox opBox, kindBox, channelBox;
    juce::Slider lo1, hi1, lo2, hi2;
    juce::Rectangle<int> panel;
};

MidiFilterEditor::MidiFilterEditor (const theme::Palette& p, const i18n::Translator& t, int track,
                                    std::function<midi::FilterDescription()> read,
                                    std::function<void (const midi::FilterDescription&)> write, std::function<void()> onClose)
{
    setWantsKeyboardFocus (true);
    impl_ = std::make_unique<Impl> (*this, p, t, track, std::move (read), std::move (write), std::move (onClose));
    startTimerHz (4);
}

MidiFilterEditor::~MidiFilterEditor() { stopTimer(); }

// Annuler / rétablir (ou une autre instance de l'éditeur) ont pu changer le filtre : on le relit quand on n'édite pas.
void MidiFilterEditor::timerCallback()
{
    auto& m = *impl_;
    const auto current = m.read();
    if (! (current == m.d) && ! m.list.isMouseButtonDown() && ! m.lo1.isMouseButtonDown() && ! m.hi1.isMouseButtonDown()
        && ! m.lo2.isMouseButtonDown() && ! m.hi2.isMouseButtonDown())
    {
        m.d = current;
        m.refresh();
    }
}

void MidiFilterEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    const auto b = impl_->panel.toFloat();
    g.setColour (impl_->colour (Token::BgCard));
    g.fillRoundedRectangle (b, 10.0f);
    g.setColour (impl_->colour (Token::Border));
    g.drawRoundedRectangle (b.reduced (0.5f), 10.0f, 1.0f);
}

void MidiFilterEditor::resized()
{
    auto& m = *impl_;
    m.panel = getLocalBounds().reduced (juce::jmax (16, getWidth() / 8), juce::jmax (12, getHeight() / 10));
    auto r = m.panel.reduced (16, 12);
    auto top = r.removeFromTop (32);
    m.closeButton.setBounds (top.removeFromRight (90));
    m.title.setBounds (top.removeFromLeft (320));
    m.enabled.setBounds (top);
    r.removeFromTop (6);
    m.hint.setBounds (r.removeFromTop (18));
    m.preview.setBounds (r.removeFromBottom (26));
    r.removeFromBottom (6);

    auto left = r.removeFromLeft (juce::jmax (260, r.getWidth() * 2 / 5));
    r.removeFromLeft (14);
    auto buttons = left.removeFromBottom (32);
    left.removeFromBottom (6);
    m.list.setBounds (left);
    const int bw = buttons.getWidth();   // « + Condition » et « + Group » ont besoin de plus de place que Up / Down
    m.addLeaf.setBounds (buttons.removeFromLeft (bw * 28 / 100).reduced (1));
    m.addGroup.setBounds (buttons.removeFromLeft (bw * 22 / 100).reduced (1));
    m.up.setBounds (buttons.removeFromLeft (bw * 15 / 100).reduced (1));
    m.down.setBounds (buttons.removeFromLeft (bw * 17 / 100).reduced (1));
    m.remove.setBounds (buttons.reduced (1));

    auto row = [&r] (int h = 28) { auto x = r.removeFromTop (h); r.removeFromTop (6); return x; };
    {
        auto x = row();
        m.opLabel.setBounds (x.removeFromLeft (110));
        m.opBox.setBounds (x.removeFromLeft (200));
    }
    {
        auto x = row();
        m.kindLabel.setBounds (x.removeFromLeft (110));
        m.kindBox.setBounds (x.removeFromLeft (240));
    }
    {
        auto x = row();
        m.channelLabel.setBounds (x.removeFromLeft (110));
        m.channelBox.setBounds (x.removeFromLeft (140));
    }
    {
        auto x = row (22);
        m.range1Label.setBounds (x);
        auto a = row();
        m.lo1.setBounds (a.removeFromLeft (a.getWidth() / 2).reduced (2, 0));
        m.hi1.setBounds (a.reduced (2, 0));
    }
    {
        auto x = row (22);
        m.range2Label.setBounds (x);
        auto a = row();
        m.lo2.setBounds (a.removeFromLeft (a.getWidth() / 2).reduced (2, 0));
        m.hi2.setBounds (a.reduced (2, 0));
    }
}

void MidiFilterEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! impl_->panel.contains (e.getPosition()) && impl_->close) impl_->close();
}

bool MidiFilterEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (impl_->close) impl_->close(); return true; }
    return false;
}

void MidiFilterEditor::parentSizeChanged()
{
    if (auto* parent = getParentComponent()) setBounds (parent->getLocalBounds());
}
