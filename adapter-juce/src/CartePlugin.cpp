#include "CartePlugin.h"

#include <algorithm>
#include <cmath>

using namespace neptune::core;
using chords::Item;

namespace
{
constexpr int kDesignW = 1000, kDesignH = 400;

// Même dossier de configuration que Neptune : le thème, le contraste et la langue sont communs à toute la suite.
std::filesystem::path configDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return std::filesystem::u8path (base.getChildFile ("Neptune").getFullPathName().toStdString());
}

const double kGrids[4] = { 0.25, 0.5, 1.0, 0.0 };   // 0 : une mesure
const char* const kGridNames[4] = { "1/16", "1/8", "1 temps", "1 mesure" };
}

// ================================================================================================ processeur

CarteProcessor::CarteProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    std::vector<std::string> warnings;
    settings_ = settings::loadFile (configDirectory() / "settings.json", &warnings);
    ticker_.startTimer (500);
}

CarteProcessor::~CarteProcessor()
{
    ticker_.stopTimer();
    writer_.close();
}

bool CarteProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void CarteProcessor::processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    // Le son passe sans changement ; on ne fait que noter la position de l'hôte pour la tête de lecture.
    bool has = false, playing = false;
    double beats = 0.0, bpb = beatsPerBar_.load (std::memory_order_relaxed);
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            playing = pos->getIsPlaying();
            if (const auto ppq = pos->getPpqPosition())
            {
                has = true;
                beats = *ppq;
            }
            if (const auto ts = pos->getTimeSignature())
                if (ts->denominator > 0 && ts->numerator > 0) bpb = ts->numerator * 4.0 / ts->denominator;
        }
    beats_.store (beats, std::memory_order_relaxed);
    beatsPerBar_.store (bpb, std::memory_order_relaxed);
    hasPosition_.store (has, std::memory_order_relaxed);
    playing_.store (playing, std::memory_order_relaxed);
}

CarteProcessor::Transport CarteProcessor::transport() const noexcept
{
    Transport t;
    t.hasPosition = hasPosition_.load (std::memory_order_relaxed);
    t.playing = playing_.load (std::memory_order_relaxed);
    t.beats = beats_.load (std::memory_order_relaxed);
    t.beatsPerBar = beatsPerBar_.load (std::memory_order_relaxed);
    return t;
}

std::vector<Item> CarteProcessor::items() const
{
    const juce::ScopedLock sl (lock_);
    return items_;
}

void CarteProcessor::setItems (std::vector<Item> v, bool publish)
{
    {
        const juce::ScopedLock sl (lock_);
        items_ = std::move (v);
    }
    if (publish) publishNow();
}

void CarteProcessor::commit (const std::vector<Item>& before, const std::vector<Item>& after, const char* name)
{
    undo_.perform (std::make_unique<SnapshotCommand<std::vector<Item>>> (before, after, [this] (const std::vector<Item>& v) { setItems (v); }, name));
}

void CarteProcessor::publishNow()
{
    if (! writer_.isOpen()) return;
    writer_.publish (chords::toContainers (items()));
}

void CarteProcessor::setChannel (int c)
{
    channel_.store (juce::jlimit (1, chordmap::kChannels, c));
    tick();
}

void CarteProcessor::tick()
{
    const int ch = channel_.load();
    if (ch != openedChannel_)
    {
        writer_.close();
        openedChannel_ = ch;
        const auto s = writer_.open (ch);
        writerStatus_.store (static_cast<int> (s));
        if (s == chordmap::Status::Ok) publishNow();
        return;
    }
    if (writer_.isOpen())
        writer_.touch();
    else if (writerStatus_.load() == static_cast<int> (chordmap::Status::InUse) && writer_.open (ch) == chordmap::Status::Ok)   // le canal s'est libéré
    {
        writerStatus_.store (static_cast<int> (chordmap::Status::Ok));
        publishNow();
    }
}

void CarteProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::MemoryOutputStream out (dest, false);
    out.writeInt (1);   // version
    out.writeInt (channel_.load());
    out.writeInt (naming_.load());
    out.writeString (juce::String::fromUTF8 (chords::toText (items()).c_str()));
}

void CarteProcessor::setStateInformation (const void* data, int size)
{
    if (size < 16) return;
    juce::MemoryInputStream in (data, static_cast<size_t> (size), false);
    if (in.readInt() != 1) return;
    const int ch = in.readInt(), nm = in.readInt();
    const auto text = in.readString();
    channel_.store (juce::jlimit (1, chordmap::kChannels, ch));
    naming_.store (nm == 1 ? 1 : 0);
    setItems (chords::fromText (text.toStdString()), false);
    undo_.clear();
    tick();   // ouvre le canal et publie
    publishNow();
}

juce::AudioProcessorEditor* CarteProcessor::createEditor() { return new CarteEditor (*this); }

// ================================================================================================ la timeline

class CarteEditor::TimelineView : public juce::Component
{
public:
    TimelineView (CarteProcessor& p, const nova::Skin& s) : proc_ (p), skin_ (s)
    {
        setWantsKeyboardFocus (true);
        edit_.setMultiLine (false);
        edit_.setSelectAllWhenFocused (true);
        edit_.onReturnKey = [this] { acceptEdit(); };
        edit_.onEscapeKey = [this] { cancelEdit(); };
        edit_.onFocusLost = [this] { cancelEdit(); };
        addChildComponent (edit_);
        items_ = p.items();
    }

    void setGrid (int g) { grid_ = g; }
    int grid() const { return grid_; }
    void zoom (double factor, double anchorBeats)
    {
        const double x = beatToX (anchorBeats);
        ppb_ = juce::jlimit (3.0, 400.0, ppb_ * factor);
        scroll_ = anchorBeats - (x - left()) / skin_.px (static_cast<float> (ppb_));
        clampScroll();
        repaint();
    }
    void zoomCentre (double factor) { zoom (factor, scroll_ + (getWidth() - left()) * 0.5 / skin_.px (static_cast<float> (ppb_))); }
    void setFollow (bool f) { follow_ = f; }
    bool follow() const { return follow_; }
    void reload() { items_ = proc_.items(); selected_ = juce::jlimit (-1, static_cast<int> (items_.size()) - 1, selected_); repaint(); }
    void tickTransport()
    {
        const auto t = proc_.transport();
        if (follow_ && t.playing && t.hasPosition)
        {
            const double span = (getWidth() - left()) / skin_.px (static_cast<float> (ppb_));
            if (t.beats < scroll_ || t.beats > scroll_ + span * 0.9) { scroll_ = std::max (0.0, t.beats - span * 0.1); }
        }
        if (t.beats != lastBeats_ || t.hasPosition != lastHas_ || t.beatsPerBar != lastBpb_) { lastBeats_ = t.beats; lastHas_ = t.hasPosition; lastBpb_ = t.beatsPerBar; repaint(); }
    }
    juce::String statusText() const
    {
        if (selected_ >= 0 && selected_ < static_cast<int> (items_.size()))
        {
            const auto& it = items_[static_cast<size_t> (selected_)];
            return juce::String::fromUTF8 (chords::formatChord (it.chord, proc_.naming()).c_str()) + juce::String::fromUTF8 ("  ·  début ") + juce::String (it.start, 2)
                   + juce::String::fromUTF8 (" temps  ·  durée ") + juce::String (it.length, 2) + " temps";
        }
        return {};
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const bool cmd = k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown();
        const int code = k.getKeyCode();
        if (cmd && (code == 'Z' || code == 'z'))
        {
            if (k.getModifiers().isShiftDown()) proc_.undo().redo(); else proc_.undo().undo();
            reload();
            return true;
        }
        if (cmd && (code == 'Y' || code == 'y')) { proc_.undo().redo(); reload(); return true; }
        if ((code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) && selected_ >= 0 && selected_ < static_cast<int> (items_.size()))
        {
            auto after = items_;
            chords::removeItem (after, selected_);
            proc_.commit (items_, after, "Supprimer l'accord");
            selected_ = -1;
            reload();
            return true;
        }
        if (code == juce::KeyPress::escapeKey) { selected_ = -1; repaint(); return true; }
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        using T = theme::Token;
        const auto b = getLocalBounds().toFloat();
        skin_.fill (g, T::PanelBg, b, skin_.px (10.0f));
        g.setColour (skin_.colour (T::Pb));
        g.drawRoundedRectangle (b.reduced (0.5f), skin_.px (10.0f), 1.0f);
        const float rulerH = skin_.px (28.0f);
        const float top = rulerH + skin_.px (8.0f), bottom = top + skin_.px (laneHeight);
        const float pxBeat = skin_.px (static_cast<float> (ppb_));
        const double bpb = proc_.transport().beatsPerBar;

        // règle : mesures et temps
        g.saveState();
        g.reduceClipRegion (getLocalBounds().withTrimmedLeft (left()));
        const double first = std::floor (scroll_), last = scroll_ + (getWidth() - left()) / pxBeat;
        for (double beat = first; beat <= last + 1.0; beat += 1.0)
        {
            const float x = static_cast<float> (beatToX (beat));
            const bool bar = bpb > 0.0 && std::fmod (beat + 1e-6, bpb) < 1e-3;
            if (bar)
            {
                g.setColour (skin_.colour (T::Grid, 1.0f));
                g.fillRect (x, rulerH - skin_.px (14.0f), 1.0f, b.getHeight() - rulerH + skin_.px (14.0f) - skin_.px (6.0f));
                g.setColour (skin_.colour (T::TextDim));
                g.setFont (skin_.font (12.0f));
                g.drawText (juce::String (static_cast<int> (std::floor (beat / bpb + 1e-6)) + 1), static_cast<int> (x + skin_.px (4.0f)), static_cast<int> (skin_.px (6.0f)), static_cast<int> (skin_.px (50.0f)),
                            static_cast<int> (skin_.px (16.0f)), juce::Justification::centredLeft, false);
            }
            else if (pxBeat >= skin_.px (14.0f))
            {
                g.setColour (skin_.colour (T::Grid, 0.55f));
                g.fillRect (x, rulerH - skin_.px (6.0f), 1.0f, b.getHeight() - rulerH + skin_.px (6.0f) - skin_.px (6.0f));
            }
        }

        g.setColour (skin_.colour (T::BgApp, 0.45f));
        g.fillRoundedRectangle (static_cast<float> (left()), top - skin_.px (4.0f), b.getWidth() - left() - skin_.px (10.0f), bottom - top + skin_.px (8.0f), skin_.px (6.0f));

        // conteneurs
        for (size_t i = 0; i < items_.size(); ++i)
        {
            const auto& it = items_[i];
            auto r = juce::Rectangle<float> (static_cast<float> (beatToX (it.start)), top, static_cast<float> (it.length) * pxBeat, bottom - top).reduced (1.0f, 0.0f);
            if (r.getRight() < left() || r.getX() > getWidth()) continue;
            const bool sel = static_cast<int> (i) == selected_;
            g.setColour (skin_.colour (T::Accent, sel ? 0.42f : 0.22f));
            g.fillRoundedRectangle (r, skin_.px (6.0f));
            g.setColour (skin_.colour (sel ? T::Text : T::Accent, sel ? 0.9f : 0.9f));
            g.drawRoundedRectangle (r, skin_.px (6.0f), sel ? 2.0f : 1.0f);
            g.saveState();
            g.reduceClipRegion (r.toNearestInt().reduced (static_cast<int> (skin_.px (6.0f)), 0));
            g.setColour (skin_.colour (T::Text));
            g.setFont (skin_.font (20.0f, true));
            g.drawText (juce::String::fromUTF8 (chords::formatChord (it.chord, proc_.naming()).c_str()), r.toNearestInt().reduced (static_cast<int> (skin_.px (8.0f)), 0), juce::Justification::centredLeft, false);
            g.restoreState();
            g.setColour (skin_.colour (T::Handle, 0.8f));   // poignée de durée
            g.fillRoundedRectangle (r.getRight() - skin_.px (5.0f), r.getCentreY() - skin_.px (12.0f), skin_.px (3.0f), skin_.px (24.0f), 1.5f);
        }
        if (items_.empty())
        {
            g.setColour (skin_.colour (T::TextDim2));
            g.setFont (skin_.font (15.0f));
            g.drawText (juce::String::fromUTF8 ("Double-clic dans la zone pour ajouter un accord (Dm7, C/E, Sib, Do, Sol7...)"), juce::Rectangle<float> (static_cast<float> (left()) + skin_.px (12.0f), top, b.getWidth() - 2 * skin_.px (12.0f), bottom - top).toNearestInt(),
                        juce::Justification::centred, true);
        }

        // tête de lecture
        const auto t = proc_.transport();
        if (t.hasPosition)
        {
            const float x = static_cast<float> (beatToX (t.beats));
            g.setColour (skin_.colour (T::Pink));
            g.fillRect (x - 0.5f, rulerH - skin_.px (14.0f), 2.0f, b.getHeight() - rulerH + skin_.px (14.0f) - skin_.px (4.0f));
            juce::Path tri;
            tri.addTriangle (x - skin_.px (6.0f), skin_.px (3.0f), x + skin_.px (7.0f), skin_.px (3.0f), x + 0.5f, rulerH - skin_.px (12.0f));
            g.fillPath (tri);
        }
        g.restoreState();
    }

    void resized() override { edit_.setBounds (edit_.getBounds()); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        cancelEdit();
        dragMode_ = DragMode::None;
        const int i = itemAt (e.position);
        selected_ = i;
        if (i >= 0)
        {
            const auto& it = items_[static_cast<size_t> (i)];
            before_ = items_;
            const double edgeX = beatToX (it.start + it.length);
            dragMode_ = (edgeX - e.position.x <= skin_.px (9.0f)) ? DragMode::Resize : DragMode::Move;
            grab_ = xToBeat (e.position.x) - it.start;
            dragIndex_ = i;
            changed_ = false;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragMode_ == DragMode::None) return;
        auto work = before_;
        int idx = dragIndex_;
        if (dragMode_ == DragMode::Move)
            idx = chords::moveItem (work, dragIndex_, std::max (0.0, snap (xToBeat (e.position.x) - grab_)));
        else
        {
            const double end = snap (xToBeat (e.position.x));
            idx = chords::resizeItem (work, dragIndex_, std::max (chords::kMinLength, end - before_[static_cast<size_t> (dragIndex_)].start));
        }
        if (idx < 0) return;
        items_ = work;
        selected_ = idx;
        changed_ = true;
        proc_.setItems (items_, false);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragMode_ != DragMode::None && changed_)
            proc_.commit (before_, items_, dragMode_ == DragMode::Move ? "Déplacer l'accord" : "Durée de l'accord");
        dragMode_ = DragMode::None;
        changed_ = false;
        reload();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int i = itemAt (e.position);
        if (i >= 0)
        {
            selected_ = i;
            const auto& it = items_[static_cast<size_t> (i)];
            openEdit (i, it.start, it.length, juce::String::fromUTF8 (chords::formatChord (it.chord, proc_.naming()).c_str()));
            return;
        }
        if (e.position.x < left() || e.position.y < skin_.px (28.0f)) return;
        const double start = std::max (0.0, snap (xToBeat (e.position.x)));
        if (chords::indexAt (items_, start) >= 0) return;
        double len = grid() > 0 && kGrids[grid_] == 0.0 ? proc_.transport().beatsPerBar : std::max (1.0, proc_.transport().beatsPerBar);
        for (const auto& o : items_)
            if (o.start > start) { len = std::min (len, o.start - start); break; }
        len = std::max (chords::kMinLength, len);
        openEdit (-1, start, len, {});
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isAltDown())
        {
            zoom (std::pow (1.25, w.deltaY * 4.0), xToBeat (e.position.x));
            return;
        }
        const double d = std::fabs (w.deltaX) > std::fabs (w.deltaY) ? w.deltaX : -w.deltaY;
        scroll_ += d * 8.0 * 40.0 / ppb_;
        clampScroll();
        repaint();
    }

private:
    enum class DragMode { None, Move, Resize };
    static constexpr float laneHeight = 100.0f;   // hauteur de la ligne d'accords (maquette)

    int left() const { return static_cast<int> (skin_.px (10.0f)); }
    double beatToX (double beat) const { return left() + (beat - scroll_) * skin_.px (static_cast<float> (ppb_)); }
    double xToBeat (double x) const { return scroll_ + (x - left()) / skin_.px (static_cast<float> (ppb_)); }
    void clampScroll() { scroll_ = std::max (0.0, scroll_); }

    double snap (double beats) const
    {
        const double g = kGrids[grid_] == 0.0 ? std::max (1.0, proc_.transport().beatsPerBar) : kGrids[grid_];
        return std::round (beats / g) * g;
    }

    int itemAt (juce::Point<float> p) const
    {
        const float top = skin_.px (36.0f), bottom = top + skin_.px (laneHeight);
        if (p.y < top || p.y > bottom) return -1;
        const double beat = xToBeat (p.x);
        return chords::indexAt (items_, beat);
    }

    void openEdit (int index, double start, double length, const juce::String& text)
    {
        editIndex_ = index;
        editStart_ = start;
        editLength_ = length;
        const float top = skin_.px (36.0f);
        const int w = std::max (static_cast<int> (skin_.px (130.0f)), static_cast<int> (length * skin_.px (static_cast<float> (ppb_))));
        edit_.setBounds (static_cast<int> (beatToX (start)), static_cast<int> (top + skin_.px (laneHeight * 0.5f - 18.0f)), w, static_cast<int> (skin_.px (36.0f)));
        edit_.setFont (skin_.font (20.0f, true));
        edit_.setColour (juce::TextEditor::backgroundColourId, skin_.colour (theme::Token::BgApp));
        edit_.setColour (juce::TextEditor::textColourId, skin_.colour (theme::Token::Text));
        edit_.setColour (juce::TextEditor::outlineColourId, skin_.colour (theme::Token::Accent));
        edit_.setColour (juce::TextEditor::focusedOutlineColourId, skin_.colour (theme::Token::Accent));
        edit_.setColour (juce::CaretComponent::caretColourId, skin_.colour (theme::Token::Accent));
        edit_.setText (text, false);
        edit_.setVisible (true);
        edit_.grabKeyboardFocus();
        edit_.selectAll();
    }

    void acceptEdit()
    {
        chords::Chord c;
        if (! chords::parseChord (edit_.getText().toStdString(), c))
        {
            edit_.setColour (juce::TextEditor::textColourId, skin_.colour (theme::Token::Pink));
            edit_.applyColourToAllText (skin_.colour (theme::Token::Pink));
            return;   // reste ouvert : l'utilisateur corrige ou annule avec Échap
        }
        auto after = items_;
        if (editIndex_ >= 0 && editIndex_ < static_cast<int> (after.size()))
            after[static_cast<size_t> (editIndex_)].chord = c;
        else
        {
            Item it;
            it.start = editStart_;
            it.length = editLength_;
            it.chord = c;
            chords::place (after, it);
        }
        const auto before = items_;
        edit_.setVisible (false);
        proc_.commit (before, after, editIndex_ >= 0 ? "Changer l'accord" : "Ajouter un accord");
        reload();
        grabKeyboardFocus();
    }

    void cancelEdit()
    {
        if (! edit_.isVisible()) return;
        edit_.setVisible (false);
        grabKeyboardFocus();
    }

    CarteProcessor& proc_;
    const nova::Skin& skin_;
    std::vector<Item> items_, before_;
    int selected_ = -1, dragIndex_ = -1, grid_ = 2;
    DragMode dragMode_ = DragMode::None;
    bool changed_ = false, follow_ = true;
    double ppb_ = 40.0, scroll_ = 0.0, grab_ = 0.0;
    double lastBeats_ = -1.0, lastBpb_ = 0.0;
    bool lastHas_ = false;
    juce::TextEditor edit_;
    int editIndex_ = -1;
    double editStart_ = 0.0, editLength_ = 4.0;
};

// ================================================================================================ l'éditeur

namespace
{
theme::Palette makePalette (const settings::GlobalSettings& st)
{
    const auto id = theme::themeFromString (st.theme).value_or (theme::ThemeId::Dark);
    return theme::withContrast (theme::definition (id), st.contrast);
}
const theme::ThemeDefinition& themeOf (const settings::GlobalSettings& st)
{
    return theme::definition (theme::themeFromString (st.theme).value_or (theme::ThemeId::Dark));
}
}

CarteEditor::CarteEditor (CarteProcessor& p)
    : juce::AudioProcessorEditor (p), proc_ (p), skin_ (makePalette (p.settings()), themeOf (p.settings()).knob, themeOf (p.settings()).id),
      channelPill_ (skin_, nova::Pill::Look::Small), gridPill_ (skin_, nova::Pill::Look::Small), namingPill_ (skin_, nova::Pill::Look::Small),
      zoomOut_ (skin_, nova::Pill::Look::Small), zoomIn_ (skin_, nova::Pill::Look::Small), followPill_ (skin_, nova::Pill::Look::Small),
      undoPill_ (skin_, nova::Pill::Look::Small), redoPill_ (skin_, nova::Pill::Look::Small)
{
    view_ = std::make_unique<TimelineView> (p, skin_);
    addAndMakeVisible (*view_);
    for (auto* c : std::initializer_list<nova::Pill*> { &channelPill_, &gridPill_, &namingPill_, &zoomOut_, &zoomIn_, &followPill_, &undoPill_, &redoPill_ }) addAndMakeVisible (c);

    channelPill_.onClick = [this] { proc_.setChannel (proc_.channel() % chordmap::kChannels + 1); refreshPills(); repaint(); };
    gridPill_.onClick = [this] { view_->setGrid ((view_->grid() + 1) % 4); refreshPills(); };
    namingPill_.onClick = [this] { proc_.setNaming (proc_.naming() == chords::Naming::Letters ? chords::Naming::French : chords::Naming::Letters); refreshPills(); view_->repaint(); };
    zoomOut_.setText (juce::String::fromUTF8 ("\xE2\x88\x92"));
    zoomOut_.onClick = [this] { view_->zoomCentre (1.0 / 1.4); };
    zoomIn_.setText ("+");
    zoomIn_.onClick = [this] { view_->zoomCentre (1.4); };
    followPill_.onClick = [this] { view_->setFollow (! view_->follow()); refreshPills(); };
    undoPill_.setText (juce::String::fromUTF8 ("Annuler"));
    undoPill_.onClick = [this] { proc_.undo().undo(); view_->reload(); refreshPills(); };
    redoPill_.setText (juce::String::fromUTF8 ("Rétablir"));
    redoPill_.onClick = [this] { proc_.undo().redo(); view_->reload(); refreshPills(); };

    nova::configureConstrainer (constrainer_, kDesignW, kDesignH, 0.6f, 3.0f);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (kDesignW, kDesignH);
    refreshPills();
    startTimerHz (30);
}

CarteEditor::~CarteEditor() { stopTimer(); }

void CarteEditor::refreshPills()
{
    channelPill_.setText (juce::String::fromUTF8 ("Canal ") + juce::String (proc_.channel()));
    gridPill_.setText (juce::String::fromUTF8 ("Grille ") + kGridNames[view_->grid()]);
    namingPill_.setText (proc_.naming() == chords::Naming::Letters ? "C D E..." : juce::String::fromUTF8 ("Do Ré Mi..."));
    followPill_.setText ("Suivre");
    followPill_.setOn (view_->follow());
    undoPill_.setDim (! proc_.undo().canUndo());
    redoPill_.setDim (! proc_.undo().canRedo());
}

void CarteEditor::layoutAndScale()
{
    skin_.setScale (nova::fitScale (getWidth(), getHeight(), kDesignW, kDesignH, 0.6f, 3.0f));
}

void CarteEditor::resized()
{
    layoutAndScale();
    const auto px = [this] (float v) { return skin_.ipx (v); };
    const int y = px (54.0f), h = px (28.0f);
    int x = px (14.0f);
    const auto put = [&] (nova::Pill& pill, float w) { pill.setBounds (x, y, px (w), h); x += px (w + 8.0f); };
    put (channelPill_, 84.0f);
    put (gridPill_, 130.0f);
    put (namingPill_, 100.0f);
    put (zoomOut_, 34.0f);
    put (zoomIn_, 34.0f);
    put (followPill_, 80.0f);
    int rx = getWidth() - px (14.0f);
    rx -= px (96.0f); redoPill_.setBounds (rx, y, px (96.0f), h);
    rx -= px (8.0f) + px (96.0f); undoPill_.setBounds (rx, y, px (96.0f), h);
    view_->setBounds (px (14.0f), px (94.0f), getWidth() - px (28.0f), getHeight() - px (94.0f) - px (40.0f));
}

void CarteEditor::paint (juce::Graphics& g)
{
    g.fillAll (skin_.colour (theme::Token::BgApp));
    nova::paintHeaderBar (g, skin_, juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), skin_.px (46.0f)), "Carte", juce::String::fromUTF8 ("timeline d'accords"));
    const float y = static_cast<float> (getHeight()) - skin_.px (34.0f);
    g.setFont (skin_.font (13.0f));
    const int status = proc_.writerStatus();
    juce::String text = hint_.isNotEmpty() ? hint_ : view_->statusText();
    if (status == static_cast<int> (chordmap::Status::InUse))
    {
        g.setColour (skin_.colour (theme::Token::Pink));
        text = juce::String::fromUTF8 ("Ce canal est déjà utilisé par une autre Carte vivante : choisissez un autre canal. Cette carte n'est pas publiée.");
    }
    else if (status != static_cast<int> (chordmap::Status::Ok))
    {
        g.setColour (skin_.colour (theme::Token::Pink));
        text = juce::String::fromUTF8 ("Carte non publiée (ouverture de la mémoire partagée impossible).");
    }
    else
    {
        g.setColour (skin_.colour (theme::Token::TextDim));
        if (text.isEmpty())
            text = juce::String::fromUTF8 ("Publiée sur le canal ") + juce::String (proc_.channel()) + juce::String::fromUTF8 (".  Double-clic : ajouter ou changer un accord  ·  glisser : déplacer  ·  bord droit : durée  ·  Suppr : effacer  ·  Ctrl+molette : zoom");
    }
    g.drawText (text, juce::Rectangle<float> (skin_.px (16.0f), y, static_cast<float> (getWidth()) - skin_.px (32.0f), skin_.px (24.0f)).toNearestInt(), juce::Justification::centredLeft, true);
}

void CarteEditor::timerCallback()
{
    view_->tickTransport();
    const auto rev = proc_.undo().revision();
    if (rev != lastUndoRevision_) { lastUndoRevision_ = rev; refreshPills(); }
    repaint (0, getHeight() - skin_.ipx (40.0f), getWidth(), skin_.ipx (40.0f));
}

bool CarteEditor::keyPressed (const juce::KeyPress& k) { return view_ != nullptr && view_->keyPressed (k); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CarteProcessor(); }
