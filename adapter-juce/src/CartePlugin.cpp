#include "CartePlugin.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace neptune::core;

namespace
{
constexpr int kDesignW = 1100, kDesignH = 440;

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

carte::Score CarteProcessor::score() const
{
    const juce::ScopedLock sl (lock_);
    return score_;
}

void CarteProcessor::setScore (carte::Score v, bool publish)
{
    {
        const juce::ScopedLock sl (lock_);
        score_ = std::move (v);
    }
    if (publish) publishNow();
}

void CarteProcessor::commit (const carte::Score& before, const carte::Score& after, const char* name)
{
    undo_.perform (std::make_unique<SnapshotCommand<carte::Score>> (before, after, [this] (const carte::Score& v) { setScore (v); }, name));
}

void CarteProcessor::publishNow()
{
    if (! writer_.isOpen()) return;
    writer_.publish (carte::toContainers (score()));
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
    out.writeInt (2);   // version : 1 = accords seuls (texte), 2 = toute la carte (JSON)
    out.writeInt (channel_.load());
    out.writeInt (naming_.load());
    out.writeString (juce::String::fromUTF8 (carte::toJson (score()).c_str()));
}

void CarteProcessor::setStateInformation (const void* data, int size)
{
    if (size < 16) return;
    juce::MemoryInputStream in (data, static_cast<size_t> (size), false);
    const int version = in.readInt();
    if (version != 1 && version != 2) return;
    const int ch = in.readInt(), nm = in.readInt();
    const auto text = in.readString();
    channel_.store (juce::jlimit (1, chordmap::kChannels, ch));
    naming_.store (nm == 1 ? 1 : 0);
    setScore (carte::fromJson (text.toStdString()), false);   // lit aussi l'ancien texte des accords seuls
    undo_.clear();
    tick();   // ouvre le canal et publie
    publishNow();
}

juce::AudioProcessorEditor* CarteProcessor::createEditor() { return new CarteEditor (*this); }

// ================================================================================================ voies et éléments

namespace
{
enum Lane { LKey = 0, LScale = 1, LChord = 2, LPattern = 3, kLanes = 4 };
const float kLaneY[kLanes] = { 36.0f, 70.0f, 118.0f, 214.0f };   // maquette, depuis le haut de la vue
const float kLaneH[kLanes] = { 28.0f, 42.0f, 88.0f, 34.0f };
const char* const kLaneNames[kLanes] = { "Tonalit\xC3\xA9", "Gamme", "Accord", "Motif" };

using carte::Score;

int countOf (const Score& s, int lane)
{
    switch (lane)
    {
        case LKey: return static_cast<int> (s.keys.size());
        case LScale: return static_cast<int> (s.scales.size());
        case LChord: return static_cast<int> (s.chords.size());
        default: return static_cast<int> (s.patterns.size());
    }
}
double startOf (const Score& s, int lane, int i)
{
    const auto u = static_cast<size_t> (i);
    switch (lane)
    {
        case LKey: return s.keys[u].start;
        case LScale: return s.scales[u].start;
        case LChord: return s.chords[u].start;
        default: return s.patterns[u].start;
    }
}
double lengthOf (const Score& s, int lane, int i)
{
    const auto u = static_cast<size_t> (i);
    switch (lane)
    {
        case LKey: return u + 1 < s.keys.size() ? s.keys[u + 1].start - s.keys[u].start : 8.0;
        case LScale: return s.scales[u].length;
        case LChord: return s.chords[u].length;
        default: return s.patterns[u].length;
    }
}
juce::String labelOf (const Score& s, int lane, int i, chords::Naming n)
{
    const auto u = static_cast<size_t> (i);
    switch (lane)
    {
        case LKey: return juce::String::fromUTF8 (carte::keyName (s.keys[u], n).c_str());
        case LScale: return juce::String::fromUTF8 (carte::scaleName (s.scales[u], n).c_str());
        case LChord: return juce::String::fromUTF8 (chords::formatChord (s.chords[u].chord, n).c_str());
        default: return juce::String::fromUTF8 ("Motif ") + juce::String (s.patterns[u].slot);
    }
}
void removeAt (Score& s, int lane, int i)
{
    const auto u = static_cast<long> (i);
    switch (lane)
    {
        case LKey: s.keys.erase (s.keys.begin() + u); break;
        case LScale: s.scales.erase (s.scales.begin() + u); break;
        case LChord: s.chords.erase (s.chords.begin() + u); break;
        default: s.patterns.erase (s.patterns.begin() + u); break;
    }
}

struct Clip
{
    int lane = LChord;
    double start = 0.0, length = 4.0;
    chords::Chord chord;
    carte::ScaleItem scale;
    carte::KeyItem key;
    carte::PatternItem pat;
};
Clip clipOf (const Score& s, int lane, int i)
{
    Clip c;
    const auto u = static_cast<size_t> (i);
    c.lane = lane;
    c.start = startOf (s, lane, i);
    c.length = lengthOf (s, lane, i);
    switch (lane)
    {
        case LKey: c.key = s.keys[u]; break;
        case LScale: c.scale = s.scales[u]; break;
        case LChord: c.chord = s.chords[u].chord; break;
        default: c.pat = s.patterns[u]; break;
    }
    return c;
}
// Pose le clip à `newStart` (la longueur peut être remplacée) ; retourne l'indice dans sa voie, ou -1.
int putClip (Score& s, const Clip& c, double newStart, double newLength = -1.0)
{
    const double len = newLength > 0.0 ? newLength : c.length;
    switch (c.lane)
    {
        case LKey: { auto k = c.key; k.start = newStart; return carte::placeKey (s.keys, k); }
        case LScale: { auto x = c.scale; x.start = newStart; x.length = len; return carte::placeScale (s.scales, x); }
        case LChord: { chords::Item it; it.start = newStart; it.length = len; it.chord = c.chord; return chords::place (s.chords, it); }
        default: { auto p = c.pat; p.start = newStart; p.length = len; return carte::placePattern (s.patterns, p); }
    }
}

std::vector<Clip> gClipboard;   // commun à toutes les instances de Carte du processus
}

// ================================================================================================ fenêtre de choix

// Colonnes de choix (fondamentales, qualités, basses... ou gammes), avec un champ de texte facultatif. Posée au-dessus de la timeline.
class CarteEditor::Popup : public juce::Component
{
public:
    struct Column
    {
        juce::String title;
        juce::StringArray items;
        int selected = 0;
        float scrollRows = 0.0f;
    };

    Popup (const nova::Skin& s, juce::String title, std::vector<Column> cols, bool withText, bool canDelete)
        : skin_ (s), title_ (std::move (title)), cols_ (std::move (cols)), withText_ (withText),
          ok_ (s, nova::Pill::Look::Small), cancel_ (s, nova::Pill::Look::Small), del_ (s, nova::Pill::Look::Small), extra_ (s, nova::Pill::Look::Small)
    {
        setWantsKeyboardFocus (true);
        ok_.setText ("OK");
        cancel_.setText ("Annuler");
        del_.setText ("Supprimer");
        ok_.onClick = [this] { accept(); };
        cancel_.onClick = [this] { if (onCancel) onCancel(); };
        del_.onClick = [this] { if (onDelete) onDelete(); };
        addAndMakeVisible (ok_);
        addAndMakeVisible (cancel_);
        if (canDelete) addAndMakeVisible (del_);
        addChildComponent (extra_);
        if (withText)
        {
            text_.setMultiLine (false);
            text_.setSelectAllWhenFocused (true);
            text_.onReturnKey = [this] { accept(); };
            text_.onEscapeKey = [this] { if (onCancel) onCancel(); };
            text_.onTextChange = [this] { if (onTextChanged) onTextChanged (text_.getText()); };
            text_.setFont (s.font (18.0f, true));
            text_.setColour (juce::TextEditor::backgroundColourId, s.colour (theme::Token::BgApp));
            text_.setColour (juce::TextEditor::textColourId, s.colour (theme::Token::Text));
            text_.setColour (juce::TextEditor::outlineColourId, s.colour (theme::Token::Accent));
            text_.setColour (juce::TextEditor::focusedOutlineColourId, s.colour (theme::Token::Accent));
            addAndMakeVisible (text_);
        }
        for (auto& c : cols_) ensureVisible (c);
    }

    std::function<void (const std::vector<int>&)> onOk;
    std::function<void()> onCancel, onDelete, onChanged, onExtra;
    std::function<void (const juce::String&)> onTextChanged;

    std::vector<Column>& columns() { return cols_; }
    int selected (size_t col) const { return cols_[col].selected; }
    void setSelected (size_t col, int row) { cols_[col].selected = row; ensureVisible (cols_[col]); repaint(); }
    void setItems (size_t col, juce::StringArray items) { cols_[col].items = std::move (items); repaint(); }
    void setText (const juce::String& t) { text_.setText (t, false); }
    void setTextBad (bool bad) { text_.applyColourToAllText (skin_.colour (bad ? theme::Token::Pink : theme::Token::Text)); }
    void setExtra (const juce::String& label) { extra_.setText (label); extra_.onClick = [this] { if (onExtra) onExtra(); }; extra_.setVisible (true); resized(); }
    void focusText() { if (withText_) { text_.grabKeyboardFocus(); text_.selectAll(); } else grabKeyboardFocus(); }

    void paint (juce::Graphics& g) override
    {
        using T = theme::Token;
        g.fillAll (juce::Colours::black.withAlpha (0.45f));
        const auto panel = panelRect().toFloat();
        skin_.fill (g, T::PanelBg, panel, skin_.px (12.0f));
        g.setColour (skin_.colour (T::Accent));
        g.drawRoundedRectangle (panel, skin_.px (12.0f), 1.5f);
        g.setColour (skin_.colour (T::Text));
        g.setFont (skin_.font (17.0f, true));
        g.drawText (title_, juce::Rectangle<int> (panelRect().getX() + skin_.ipx (16.0f), panelRect().getY() + skin_.ipx (8.0f), panelRect().getWidth() - skin_.ipx (32.0f), skin_.ipx (28.0f)), juce::Justification::centredLeft);
        for (size_t c = 0; c < cols_.size(); ++c)
        {
            const auto area = columnRect (c);
            g.setColour (skin_.colour (T::BgApp, 0.5f));
            g.fillRoundedRectangle (area.toFloat(), skin_.px (6.0f));
            g.setColour (skin_.colour (T::TextDim));
            g.setFont (skin_.font (12.0f));
            g.drawText (cols_[c].title, area.getX(), area.getY() - skin_.ipx (18.0f), area.getWidth(), skin_.ipx (16.0f), juce::Justification::centredLeft);
            g.saveState();
            g.reduceClipRegion (area);
            const int rh = rowHeight();
            for (int r = 0; r < cols_[c].items.size(); ++r)
            {
                const int y = area.getY() + static_cast<int> ((static_cast<float> (r) - cols_[c].scrollRows) * rh);
                if (y + rh < area.getY() || y > area.getBottom()) continue;
                const auto row = juce::Rectangle<int> (area.getX() + 2, y, area.getWidth() - 4, rh);
                const bool sel = r == cols_[c].selected;
                if (sel)
                {
                    skin_.fillActive (g, row.toFloat(), skin_.px (5.0f));
                    g.setColour (skin_.colour (T::Accent));
                    g.drawRoundedRectangle (row.toFloat().reduced (0.5f), skin_.px (5.0f), 1.0f);
                }
                g.setColour (skin_.colour (sel ? T::Text : T::TextDim));
                g.setFont (skin_.font (15.0f, sel));
                g.drawText (cols_[c].items[r], row.reduced (skin_.ipx (8.0f), 0), juce::Justification::centredLeft, true);
            }
            g.restoreState();
        }
    }

    void resized() override
    {
        const auto p = panelRect();
        const int bh = skin_.ipx (30.0f), by = p.getBottom() - bh - skin_.ipx (12.0f);
        int x = p.getRight() - skin_.ipx (16.0f);
        x -= skin_.ipx (90.0f); ok_.setBounds (x, by, skin_.ipx (90.0f), bh);
        x -= skin_.ipx (8.0f) + skin_.ipx (100.0f); cancel_.setBounds (x, by, skin_.ipx (100.0f), bh);
        x -= skin_.ipx (8.0f) + skin_.ipx (110.0f); del_.setBounds (x, by, skin_.ipx (110.0f), bh);
        extra_.setBounds (p.getX() + skin_.ipx (16.0f), by, skin_.ipx (150.0f), bh);
        if (withText_) text_.setBounds (p.getX() + skin_.ipx (16.0f), p.getY() + skin_.ipx (42.0f), p.getWidth() - skin_.ipx (32.0f), skin_.ipx (34.0f));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (onCancel) onCancel(); return true; }
        if (k == juce::KeyPress::returnKey) { accept(); return true; }
        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (size_t c = 0; c < cols_.size(); ++c)
            if (columnRect (c).contains (e.getPosition()))
            {
                const int row = static_cast<int> (std::floor (static_cast<float> (e.y - columnRect (c).getY()) / rowHeight() + cols_[c].scrollRows));
                if (row >= 0 && row < cols_[c].items.size())
                {
                    cols_[c].selected = row;
                    repaint();
                    if (onChanged) onChanged();
                }
                return;
            }
        if (! panelRect().contains (e.getPosition()) && onCancel) onCancel();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        for (size_t c = 0; c < cols_.size(); ++c)
            if (columnRect (c).contains (e.getPosition())) { accept(); return; }
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        for (size_t c = 0; c < cols_.size(); ++c)
            if (columnRect (c).contains (e.getPosition()))
            {
                const float maxScroll = std::max (0.0f, static_cast<float> (cols_[c].items.size()) - static_cast<float> (columnRect (c).getHeight()) / rowHeight());
                cols_[c].scrollRows = juce::jlimit (0.0f, maxScroll, cols_[c].scrollRows - w.deltaY * 6.0f);
                repaint();
                return;
            }
    }

private:
    int rowHeight() const { return skin_.ipx (27.0f); }
    juce::Rectangle<int> panelRect() const
    {
        const int w = std::min (getWidth() - skin_.ipx (30.0f), static_cast<int> (cols_.size()) * skin_.ipx (230.0f) + skin_.ipx (32.0f));
        const int h = std::min (getHeight() - skin_.ipx (30.0f), skin_.ipx (520.0f));
        return juce::Rectangle<int> (w, h).withCentre (getLocalBounds().getCentre());
    }
    juce::Rectangle<int> columnRect (size_t c) const
    {
        const auto p = panelRect();
        const int top = p.getY() + skin_.ipx (withText_ ? 104.0f : 62.0f), bottom = p.getBottom() - skin_.ipx (56.0f);
        const int gap = skin_.ipx (10.0f), n = static_cast<int> (cols_.size());
        const int w = (p.getWidth() - skin_.ipx (32.0f) - gap * (n - 1)) / n;
        return juce::Rectangle<int> (p.getX() + skin_.ipx (16.0f) + static_cast<int> (c) * (w + gap), top, w, bottom - top);
    }
    void ensureVisible (Column& c)
    {
        const float rows = 10.0f;
        if (static_cast<float> (c.selected) < c.scrollRows) c.scrollRows = static_cast<float> (c.selected);
        else if (static_cast<float> (c.selected) > c.scrollRows + rows - 1.0f) c.scrollRows = static_cast<float> (c.selected) - rows + 1.0f;
        c.scrollRows = std::max (0.0f, c.scrollRows);
    }
    void accept()
    {
        std::vector<int> sel;
        for (const auto& c : cols_) sel.push_back (c.selected);
        if (onOk) onOk (sel);
    }

    const nova::Skin& skin_;
    juce::String title_;
    std::vector<Column> cols_;
    bool withText_;
    juce::TextEditor text_;
    nova::Pill ok_, cancel_, del_, extra_;
};

// ================================================================================================ la timeline

class CarteEditor::TimelineView : public juce::Component
{
public:
    TimelineView (CarteProcessor& p, const nova::Skin& s, CarteEditor& owner) : proc_ (p), skin_ (s), owner_ (owner) { setWantsKeyboardFocus (true); score_ = p.score(); }

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
    void reload() { score_ = proc_.score(); pruneSelection(); repaint(); }
    bool hasSelection() const { return ! sel_.empty(); }
    double playBeats() const { auto t = proc_.transport(); return t.hasPosition ? t.beats : 0.0; }

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
        if (sel_.size() == 1)
        {
            const auto [lane, start] = *sel_.begin();
            const int i = indexOf (lane, start);
            if (i >= 0)
                return juce::String (kLaneNames[lane]) + " : " + labelOf (score_, lane, i, proc_.naming()) + juce::String::fromUTF8 ("  ·  début ") + juce::String (start, 2)
                       + juce::String::fromUTF8 (" temps  ·  durée ") + juce::String (lengthOf (score_, lane, i), 2) + " temps";
        }
        if (sel_.size() > 1) return juce::String (static_cast<int> (sel_.size())) + juce::String::fromUTF8 (" éléments sélectionnés  ·  Ctrl+C copier  ·  Ctrl+V coller  ·  Ctrl+D dupliquer  ·  Suppr");
        return {};
    }

    // ---- actions (aussi appelées par les boutons de la barre)
    void copySelection()
    {
        gClipboard.clear();
        for (const auto& [lane, start] : sel_)
        {
            const int i = indexOf (lane, start);
            if (i >= 0) gClipboard.push_back (clipOf (score_, lane, i));
        }
    }
    void pasteAt (double at)
    {
        if (gClipboard.empty()) return;
        double minStart = 1e300;
        for (const auto& c : gClipboard) minStart = std::min (minStart, c.start);
        auto after = score_;
        std::set<std::pair<int, double>> pasted;
        for (const auto& c : gClipboard)
        {
            const double s = std::max (0.0, at + (c.start - minStart));
            if (putClip (after, c, s) >= 0) pasted.insert ({ c.lane, s });
        }
        proc_.commit (score_, after, "Coller");
        sel_ = pasted;
        reload();
    }
    void pastePoint() { pasteAt (pasteBeat()); }
    void duplicateSelection()
    {
        if (sel_.empty()) return;
        copySelection();
        double end = 0.0;
        for (const auto& c : gClipboard) end = std::max (end, c.start + c.length);
        pasteAt (end);
    }
    void deleteSelection()
    {
        if (sel_.empty()) return;
        auto after = score_;
        for (int lane = 0; lane < kLanes; ++lane)
            for (int i = countOf (after, lane) - 1; i >= 0; --i)
                if (sel_.count ({ lane, startOf (after, lane, i) })) removeAt (after, lane, i);
        proc_.commit (score_, after, "Supprimer");
        sel_.clear();
        reload();
    }
    void selectAll()
    {
        sel_.clear();
        for (int lane = 0; lane < kLanes; ++lane)
            for (int i = 0; i < countOf (score_, lane); ++i) sel_.insert ({ lane, startOf (score_, lane, i) });
        repaint();
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
        if (cmd && (code == 'C' || code == 'c')) { copySelection(); return true; }
        if (cmd && (code == 'V' || code == 'v')) { pastePoint(); return true; }
        if (cmd && (code == 'D' || code == 'd')) { duplicateSelection(); return true; }
        if (cmd && (code == 'A' || code == 'a')) { selectAll(); return true; }
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        {
            nudge (code == juce::KeyPress::rightKey ? 1.0 : -1.0, k.getModifiers().isShiftDown());   // flèches : déplacer ; Maj + flèches : changer la fin
            return true;
        }
        if (code == juce::KeyPress::escapeKey) { sel_.clear(); repaint(); return true; }
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
        const float pxBeat = skin_.px (static_cast<float> (ppb_));
        const double bpb = proc_.transport().beatsPerBar;
        const float bottomEdge = skin_.px (kLaneY[LPattern] + kLaneH[LPattern] + 8.0f);

        g.saveState();
        g.reduceClipRegion (getLocalBounds().withTrimmedLeft (left()));
        const double first = std::floor (scroll_), last = scroll_ + (getWidth() - left()) / pxBeat;
        // fonds des voies
        for (int lane = 0; lane < kLanes; ++lane)
        {
            g.setColour (skin_.colour (T::BgApp, 0.45f));
            g.fillRoundedRectangle (static_cast<float> (left()), skin_.px (kLaneY[lane]), b.getWidth() - left() - skin_.px (10.0f), skin_.px (kLaneH[lane]), skin_.px (6.0f));
            g.setColour (skin_.colour (T::TextDim2, 0.8f));
            g.setFont (skin_.font (10.0f));
            g.drawText (juce::String::fromUTF8 (kLaneNames[lane]), static_cast<int> (left() + skin_.px (4.0f)), static_cast<int> (skin_.px (kLaneY[lane]) + skin_.px (1.0f)), static_cast<int> (skin_.px (90.0f)), static_cast<int> (skin_.px (12.0f)), juce::Justification::centredLeft, false);
        }
        // règle : barres de mesure (celles du fichier si on en a, sinon celles de l'hôte) et temps
        auto barLine = [&] (double beat, const juce::String& label)
        {
            const float x = static_cast<float> (beatToX (beat));
            g.setColour (skin_.colour (T::Grid, 1.0f));
            g.fillRect (x, rulerH - skin_.px (14.0f), 1.0f, bottomEdge - rulerH + skin_.px (14.0f));
            g.setColour (skin_.colour (T::TextDim));
            g.setFont (skin_.font (12.0f));
            g.drawText (label, static_cast<int> (x + skin_.px (4.0f)), static_cast<int> (skin_.px (6.0f)), static_cast<int> (skin_.px (50.0f)), static_cast<int> (skin_.px (16.0f)), juce::Justification::centredLeft, false);
        };
        if (! score_.measures.empty())
        {
            for (size_t i = 0; i < score_.measures.size(); ++i)
            {
                const auto& m = score_.measures[i];
                if (m.start + m.length < first || m.start > last + 1.0) continue;
                barLine (m.start, juce::String (static_cast<int> (i) + 1));
                if (pxBeat >= skin_.px (14.0f))
                    for (double t = m.start + 1.0; t < m.start + m.length - 1e-6; t += 1.0)
                    {
                        g.setColour (skin_.colour (T::Grid, 0.55f));
                        g.fillRect (static_cast<float> (beatToX (t)), rulerH - skin_.px (6.0f), 1.0f, bottomEdge - rulerH + skin_.px (6.0f));
                    }
            }
        }
        else
            for (double beat = first; beat <= last + 1.0; beat += 1.0)
            {
                const bool bar = bpb > 0.0 && std::fmod (beat + 1e-6, bpb) < 1e-3;
                if (bar) barLine (beat, juce::String (static_cast<int> (std::floor (beat / bpb + 1e-6)) + 1));
                else if (pxBeat >= skin_.px (14.0f))
                {
                    g.setColour (skin_.colour (T::Grid, 0.55f));
                    g.fillRect (static_cast<float> (beatToX (beat)), rulerH - skin_.px (6.0f), 1.0f, bottomEdge - rulerH + skin_.px (6.0f));
                }
            }

        // tonalité globale (avant le premier marqueur)
        if (score_.hasGlobalKey)
        {
            const double end = score_.keys.empty() ? 1.0e6 : score_.keys.front().start;
            auto r = juce::Rectangle<float> (static_cast<float> (beatToX (0.0)), skin_.px (kLaneY[LKey]), static_cast<float> (end) * pxBeat, skin_.px (kLaneH[LKey])).reduced (1.0f, 2.0f);
            g.setColour (skin_.colour (T::Pink, 0.10f));
            g.fillRoundedRectangle (r, skin_.px (5.0f));
            g.setColour (skin_.colour (T::TextDim));
            g.setFont (skin_.font (13.0f));
            g.drawText (juce::String::fromUTF8 (carte::keyName (score_.globalKey, proc_.naming()).c_str()) + juce::String::fromUTF8 ("  (globale)"), r.toNearestInt().reduced (static_cast<int> (skin_.px (8.0f)), 0), juce::Justification::centredLeft, false);
        }

        // éléments des quatre voies
        const T tones[kLanes] = { T::Pink, T::Accent2, T::Accent, T::Pink2 };
        const float fonts[kLanes] = { 14.0f, 15.0f, 20.0f, 14.0f };
        for (int lane = 0; lane < kLanes; ++lane)
            for (int i = 0; i < countOf (score_, lane); ++i)
            {
                const double st = startOf (score_, lane, i), len = lengthOf (score_, lane, i);
                auto r = juce::Rectangle<float> (static_cast<float> (beatToX (st)), skin_.px (kLaneY[lane]), static_cast<float> (len) * pxBeat, skin_.px (kLaneH[lane])).reduced (1.0f, 2.0f);
                if (r.getRight() < left() || r.getX() > getWidth()) continue;
                const bool sel = sel_.count ({ lane, st }) > 0;
                g.setColour (skin_.colour (tones[lane], sel ? 0.45f : 0.22f));
                g.fillRoundedRectangle (r, skin_.px (6.0f));
                g.setColour (skin_.colour (sel ? T::Text : tones[lane], 0.95f));
                g.drawRoundedRectangle (r, skin_.px (6.0f), sel ? 2.0f : 1.0f);
                g.saveState();
                g.reduceClipRegion (r.toNearestInt().reduced (static_cast<int> (skin_.px (6.0f)), 0));
                g.setColour (skin_.colour (T::Text));
                g.setFont (skin_.font (fonts[lane], lane == LChord));
                g.drawText (labelOf (score_, lane, i, proc_.naming()), r.toNearestInt().reduced (static_cast<int> (skin_.px (8.0f)), 0), juce::Justification::centredLeft, false);
                g.restoreState();
                if (lane != LKey)
                {
                    g.setColour (skin_.colour (T::Handle, 0.8f));   // poignées de durée : à droite (la fin) et à gauche (le début)
                    g.fillRoundedRectangle (r.getRight() - skin_.px (5.0f), r.getCentreY() - skin_.px (8.0f), skin_.px (3.0f), skin_.px (16.0f), 1.5f);
                    g.fillRoundedRectangle (r.getX() + skin_.px (2.0f), r.getCentreY() - skin_.px (8.0f), skin_.px (3.0f), skin_.px (16.0f), 1.5f);
                }
            }
        if (score_.chords.empty())
        {
            g.setColour (skin_.colour (T::TextDim2));
            g.setFont (skin_.font (15.0f));
            g.drawText (juce::String::fromUTF8 ("Double-clic dans une voie : ajouter (accord, gamme, tonalité, motif).  Importer : pistes d'accords de Scorsaire."),
                        juce::Rectangle<float> (static_cast<float> (left()) + skin_.px (12.0f), skin_.px (kLaneY[LChord]), b.getWidth() - 2 * skin_.px (12.0f), skin_.px (kLaneH[LChord])).toNearestInt(), juce::Justification::centred, true);
        }
        // sélection au rectangle
        if (band_)
        {
            g.setColour (skin_.colour (T::Accent, 0.18f));
            g.fillRect (bandRect_);
            g.setColour (skin_.colour (T::Accent));
            g.drawRect (bandRect_, 1.0f);
        }

        // tête de lecture
        const auto t = proc_.transport();
        if (t.hasPosition)
        {
            const float x = static_cast<float> (beatToX (t.beats));
            g.setColour (skin_.colour (T::Pink));
            g.fillRect (x - 0.5f, rulerH - skin_.px (14.0f), 2.0f, bottomEdge - rulerH + skin_.px (14.0f));
            juce::Path tri;
            tri.addTriangle (x - skin_.px (6.0f), skin_.px (3.0f), x + skin_.px (7.0f), skin_.px (3.0f), x + 0.5f, rulerH - skin_.px (12.0f));
            g.fillPath (tri);
        }
        // point de collage
        if (hasCursor_)
        {
            g.setColour (skin_.colour (T::Text, 0.5f));
            const float x = static_cast<float> (beatToX (cursor_));
            g.drawLine (x, skin_.px (kLaneY[LKey]), x, bottomEdge, 1.0f);
        }
        g.restoreState();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        dragMode_ = DragMode::None;
        band_ = false;
        const auto [lane, i] = hit (e.position);
        if (lane >= 0) { cursor_ = std::max (0.0, snap (xToBeat (e.position.x))); hasCursor_ = true; }
        if (lane >= 0 && i >= 0)
        {
            const double st = startOf (score_, lane, i);
            const bool toggle = e.mods.isShiftDown() || e.mods.isCtrlDown() || e.mods.isCommandDown();
            if (toggle) { if (! sel_.erase ({ lane, st })) sel_.insert ({ lane, st }); }
            else if (! sel_.count ({ lane, st })) { sel_.clear(); sel_.insert ({ lane, st }); }
            before_ = score_;
            primary_ = { lane, st };
            const double edgeX = beatToX (st + lengthOf (score_, lane, i)), startX = beatToX (st);
            const float grip = skin_.px (9.0f);
            dragMode_ = DragMode::Move;
            if (lane != LKey && ! toggle)
            {
                const bool nearEnd = edgeX - e.position.x <= grip, nearStart = e.position.x - startX <= grip;
                if (nearEnd && (! nearStart || edgeX - e.position.x <= e.position.x - startX)) dragMode_ = DragMode::Resize;
                else if (nearStart) dragMode_ = DragMode::ResizeLeft;
            }
            grab_ = xToBeat (e.position.x) - st;
            changed_ = false;
            if (dragMode_ != DragMode::Move) { sel_.clear(); sel_.insert ({ lane, st }); }
            dragSel_ = sel_;
        }
        else if (e.position.y >= skin_.px (30.0f))
        {
            if (! e.mods.isShiftDown()) sel_.clear();
            band_ = true;
            bandStart_ = e.position;
            bandRect_ = juce::Rectangle<float> (e.position, e.position);
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (band_)
        {
            bandRect_ = juce::Rectangle<float> (bandStart_, e.position);
            repaint();
            return;
        }
        if (dragMode_ == DragMode::None) return;
        auto work = before_;
        const auto [plane, pstart] = primary_;
        const int pi = indexIn (before_, plane, pstart);
        if (pi < 0) return;
        if (dragMode_ == DragMode::Move)
            work = movedBy (before_, dragSel_, snap (xToBeat (e.position.x) - grab_) - pstart, newSel_);   // la sélection de départ : `sel_` suit l'élément pendant le glissement
        else if (dragMode_ == DragMode::ResizeLeft)
        {
            const auto c = clipOf (work, plane, pi);
            const double end = c.start + c.length;
            const double newStart = std::min (std::max (0.0, snap (xToBeat (e.position.x))), end - carte::kMinLength);
            removeAt (work, plane, pi);
            putClip (work, c, newStart, end - newStart);
            newSel_ = { { plane, newStart } };
        }
        else
        {
            const double end = snap (xToBeat (e.position.x));
            const auto c = clipOf (work, plane, pi);
            removeAt (work, plane, pi);
            putClip (work, c, c.start, std::max (carte::kMinLength, end - c.start));
            newSel_ = { { plane, pstart } };
        }
        score_ = work;
        sel_ = newSel_;
        changed_ = true;
        proc_.setScore (score_, false);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (band_)
        {
            band_ = false;
            const double b0 = xToBeat (bandRect_.getX()), b1 = xToBeat (bandRect_.getRight());
            for (int lane = 0; lane < kLanes; ++lane)
            {
                const float y0 = skin_.px (kLaneY[lane]), y1 = y0 + skin_.px (kLaneH[lane]);
                if (bandRect_.getBottom() < y0 || bandRect_.getY() > y1) continue;
                for (int i = 0; i < countOf (score_, lane); ++i)
                {
                    const double st = startOf (score_, lane, i), en = st + lengthOf (score_, lane, i);
                    if (en >= b0 && st <= b1) sel_.insert ({ lane, st });
                }
            }
            repaint();
            return;
        }
        if (dragMode_ != DragMode::None && changed_)
            proc_.commit (before_, score_, dragMode_ == DragMode::Move ? "Déplacer" : "Changer la durée");
        dragMode_ = DragMode::None;
        changed_ = false;
        reload();
        juce::ignoreUnused (e);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto [lane, i] = hit (e.position);
        bool edge = false;
        if (lane >= 0 && lane != LKey && i >= 0)
        {
            const double st = startOf (score_, lane, i), en = st + lengthOf (score_, lane, i);
            const float grip = skin_.px (9.0f);
            edge = beatToX (en) - e.position.x <= grip || e.position.x - beatToX (st) <= grip;
        }
        setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor : (i >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto [lane, i] = hit (e.position);
        if (lane < 0) return;
        if (i >= 0)
        {
            sel_ = { { lane, startOf (score_, lane, i) } };
            open (lane, startOf (score_, lane, i), lengthOf (score_, lane, i), i);
            return;
        }
        const double start = std::max (0.0, snap (xToBeat (e.position.x)));
        double len = std::max (1.0, defaultLength (start));
        // jusqu'à l'élément suivant de la voie, sans le recouvrir
        for (int k = 0; k < countOf (score_, lane); ++k)
            if (startOf (score_, lane, k) > start) { len = std::min (len, startOf (score_, lane, k) - start); break; }
        len = std::max (carte::kMinLength, len);
        open (lane, start, len, -1);
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
    enum class DragMode { None, Move, Resize, ResizeLeft };

    int left() const { return static_cast<int> (skin_.px (10.0f)); }
    double beatToX (double beat) const { return left() + (beat - scroll_) * skin_.px (static_cast<float> (ppb_)); }
    double xToBeat (double x) const { return scroll_ + (x - left()) / skin_.px (static_cast<float> (ppb_)); }
    void clampScroll() { scroll_ = std::max (0.0, scroll_); }

    // longueur d'une mesure à `beat` : celle du fichier si on en a une, sinon celle de l'hôte
    double measureLengthAt (double beat) const
    {
        for (const auto& m : score_.measures) if (beat >= m.start && beat < m.start + m.length) return m.length;
        return std::max (1.0, proc_.transport().beatsPerBar);
    }
    double defaultLength (double beat) const { return measureLengthAt (beat); }

    double snap (double beats) const
    {
        if (kGrids[grid_] == 0.0)   // la mesure : la barre la plus proche
        {
            if (! score_.measures.empty())
            {
                double best = score_.measures.front().start, bd = std::fabs (beats - best);
                for (const auto& m : score_.measures)
                    if (std::fabs (beats - m.start) < bd) { bd = std::fabs (beats - m.start); best = m.start; }
                return best;
            }
            const double bar = std::max (1.0, proc_.transport().beatsPerBar);
            return std::round (beats / bar) * bar;
        }
        return std::round (beats / kGrids[grid_]) * kGrids[grid_];
    }

    double pasteBeat() const
    {
        const auto t = proc_.transport();
        if (t.hasPosition && t.playing) return t.beats;
        if (hasCursor_) return cursor_;
        return t.hasPosition ? t.beats : 0.0;
    }

    // `base` avec la sélection décalée de `delta` temps (jamais avant 0) ; `now` reçoit la sélection à ses nouvelles places
    Score movedBy (const Score& base, const std::set<std::pair<int, double>>& selection, double delta, std::set<std::pair<int, double>>& now) const
    {
        auto work = base;
        std::vector<Clip> moved;
        for (int lane = 0; lane < kLanes; ++lane)
            for (int i = countOf (work, lane) - 1; i >= 0; --i)
                if (selection.count ({ lane, startOf (work, lane, i) }))
                {
                    moved.push_back (clipOf (work, lane, i));
                    removeAt (work, lane, i);
                }
        double minStart = 1e300;
        for (const auto& c : moved) minStart = std::min (minStart, c.start);
        delta = std::max (delta, -minStart);
        now.clear();
        for (const auto& c : moved)
            if (putClip (work, c, c.start + delta) >= 0) now.insert ({ c.lane, c.start + delta });
        return work;
    }

    double gridStep() const { return kGrids[grid_] == 0.0 ? measureLengthAt (pasteBeat()) : kGrids[grid_]; }

    void nudge (double direction, bool resizeEnd)
    {
        if (sel_.empty()) return;
        const double step = direction * gridStep();
        auto after = score_;
        std::set<std::pair<int, double>> now;
        if (! resizeEnd) after = movedBy (score_, sel_, step, now);
        else
        {
            for (const auto& [lane, start] : sel_)
            {
                const int i = indexIn (after, lane, start);
                if (i < 0 || lane == LKey) { now.insert ({ lane, start }); continue; }
                const auto c = clipOf (after, lane, i);
                removeAt (after, lane, i);
                putClip (after, c, c.start, std::max (carte::kMinLength, c.length + step));
                now.insert ({ lane, start });
            }
        }
        proc_.commit (score_, after, resizeEnd ? "Changer la durée" : "Déplacer");
        sel_ = now;
        reload();
    }

    int indexIn (const Score& s, int lane, double start) const
    {
        for (int i = 0; i < countOf (s, lane); ++i) if (startOf (s, lane, i) == start) return i;
        return -1;
    }
    int indexOf (int lane, double start) const { return indexIn (score_, lane, start); }
    void pruneSelection()
    {
        std::set<std::pair<int, double>> keep;
        for (const auto& k : sel_) if (indexOf (k.first, k.second) >= 0) keep.insert (k);
        sel_ = keep;
    }

    // (voie, indice de l'élément sous le pointeur ou -1) ; voie -1 hors des voies
    std::pair<int, int> hit (juce::Point<float> p) const
    {
        for (int lane = 0; lane < kLanes; ++lane)
        {
            const float y0 = skin_.px (kLaneY[lane]), y1 = y0 + skin_.px (kLaneH[lane]);
            if (p.y < y0 || p.y > y1 || p.x < left()) continue;
            const double beat = xToBeat (p.x);
            for (int i = 0; i < countOf (score_, lane); ++i)
                if (beat >= startOf (score_, lane, i) && beat < startOf (score_, lane, i) + lengthOf (score_, lane, i)) return { lane, i };
            return { lane, -1 };
        }
        return { -1, -1 };
    }

    void open (int lane, double start, double length, int index)
    {
        switch (lane)
        {
            case LKey: owner_.showKeyPicker (start, index); break;
            case LScale: owner_.showScalePicker (start, length, index); break;
            case LChord: owner_.showChordBuilder (start, length, index); break;
            default: owner_.showPatternPicker (start, length, index); break;
        }
    }

    CarteProcessor& proc_;
    const nova::Skin& skin_;
    CarteEditor& owner_;
    Score score_, before_;
    std::set<std::pair<int, double>> sel_, newSel_, dragSel_;
    std::pair<int, double> primary_ { 0, 0.0 };
    int grid_ = 2;
    DragMode dragMode_ = DragMode::None;
    bool changed_ = false, follow_ = true, band_ = false, hasCursor_ = false;
    double ppb_ = 40.0, scroll_ = 0.0, grab_ = 0.0, cursor_ = 0.0;
    double lastBeats_ = -1.0, lastBpb_ = 0.0;
    bool lastHas_ = false;
    juce::Point<float> bandStart_;
    juce::Rectangle<float> bandRect_;
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
juce::StringArray rootNames (bool sharp, chords::Naming n)
{
    juce::StringArray a;
    for (int pc = 0; pc < 12; ++pc)
    {
        chords::Chord c;
        c.root = pc;
        c.sharp = sharp;
        c.rel = 0x091;   // majeur : le nom s'écrit sans suffixe
        a.add (juce::String::fromUTF8 (chords::formatChord (c, n).c_str()));
    }
    return a;
}
}

CarteEditor::CarteEditor (CarteProcessor& p)
    : juce::AudioProcessorEditor (p), proc_ (p), skin_ (makePalette (p.settings()), themeOf (p.settings()).knob, themeOf (p.settings()).id),
      channelPill_ (skin_, nova::Pill::Look::Small), gridPill_ (skin_, nova::Pill::Look::Small), namingPill_ (skin_, nova::Pill::Look::Small),
      zoomOut_ (skin_, nova::Pill::Look::Small), zoomIn_ (skin_, nova::Pill::Look::Small), followPill_ (skin_, nova::Pill::Look::Small),
      undoPill_ (skin_, nova::Pill::Look::Small), redoPill_ (skin_, nova::Pill::Look::Small),
      copyPill_ (skin_, nova::Pill::Look::Small), pastePill_ (skin_, nova::Pill::Look::Small), dupPill_ (skin_, nova::Pill::Look::Small),
      importPill_ (skin_, nova::Pill::Look::Small), keyPill_ (skin_, nova::Pill::Look::Small), clearBarsPill_ (skin_, nova::Pill::Look::Small)
{
    view_ = std::make_unique<TimelineView> (p, skin_, *this);
    addAndMakeVisible (*view_);
    for (auto* c : std::initializer_list<nova::Pill*> { &channelPill_, &gridPill_, &namingPill_, &zoomOut_, &zoomIn_, &followPill_, &undoPill_, &redoPill_,
                                                       &copyPill_, &pastePill_, &dupPill_, &importPill_, &keyPill_, &clearBarsPill_ }) addAndMakeVisible (c);

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
    copyPill_.setText ("Copier");
    copyPill_.onClick = [this] { view_->copySelection(); };
    pastePill_.setText ("Coller");
    pastePill_.onClick = [this] { view_->pastePoint(); };
    dupPill_.setText ("Dupliquer");
    dupPill_.onClick = [this] { view_->duplicateSelection(); };
    importPill_.setText (juce::String::fromUTF8 ("Importer des accords..."));
    importPill_.onClick = [this] { importFile(); };
    keyPill_.onClick = [this] { showGlobalKeyPicker(); };
    clearBarsPill_.setText (juce::String::fromUTF8 ("Retirer les barres du fichier"));
    clearBarsPill_.onClick = [this]
    {
        auto before = proc_.score(), after = before;
        after.measures.clear();
        proc_.commit (before, after, "Retirer les mesures");
        view_->reload();
        refreshPills();
    };

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
    const auto sc = proc_.score();
    keyPill_.setText (juce::String::fromUTF8 ("Tonalité globale : ") + (sc.hasGlobalKey ? juce::String::fromUTF8 (carte::keyName (sc.globalKey, proc_.naming()).c_str()) : juce::String::fromUTF8 ("aucune")));
    clearBarsPill_.setVisible (! sc.measures.empty());
}

void CarteEditor::layoutAndScale()
{
    skin_.setScale (nova::fitScale (getWidth(), getHeight(), kDesignW, kDesignH, 0.6f, 3.0f));
}

void CarteEditor::resized()
{
    layoutAndScale();
    const auto px = [this] (float v) { return skin_.ipx (v); };
    const int h = px (28.0f);
    int y = px (54.0f);
    int x = px (14.0f);
    const auto put = [&] (nova::Pill& pill, float w) { pill.setBounds (x, y, px (w), h); x += px (w + 8.0f); };
    put (channelPill_, 84.0f);
    put (gridPill_, 130.0f);
    put (namingPill_, 100.0f);
    put (zoomOut_, 34.0f);
    put (zoomIn_, 34.0f);
    put (followPill_, 80.0f);
    put (copyPill_, 80.0f);
    put (pastePill_, 80.0f);
    put (dupPill_, 100.0f);
    int rx = getWidth() - px (14.0f);
    rx -= px (96.0f); redoPill_.setBounds (rx, y, px (96.0f), h);
    rx -= px (8.0f) + px (96.0f); undoPill_.setBounds (rx, y, px (96.0f), h);
    y = px (88.0f);
    x = px (14.0f);
    put (importPill_, 190.0f);
    put (keyPill_, 290.0f);
    put (clearBarsPill_, 230.0f);
    view_->setBounds (px (14.0f), px (126.0f), getWidth() - px (28.0f), getHeight() - px (126.0f) - px (40.0f));
    if (popup_) popup_->setBounds (getLocalBounds());
}

void CarteEditor::closePopup()
{
    // la fenêtre peut se fermer depuis l'un de ses propres boutons : on ne la détruit qu'après le retour du gestionnaire d'événement
    if (popup_)
    {
        auto* p = popup_.release();
        removeChildComponent (p);
        juce::MessageManager::callAsync ([p] { delete p; });
    }
    view_->grabKeyboardFocus();
}

namespace
{
// applique `edit` à la carte et enregistre un changement annulable
template <typename F>
void applyEdit (CarteProcessor& proc, const char* name, F edit)
{
    const auto before = proc.score();
    auto after = before;
    edit (after);
    proc.commit (before, after, name);
}
}

void CarteEditor::showChordBuilder (double start, double length, int index)
{
    closePopup();
    const auto naming = proc_.naming();
    struct State { int root = 0, bass = -1, quality = 0; bool sharp = false; };
    auto st = std::make_shared<State>();
    const auto sc = proc_.score();
    if (index >= 0 && index < static_cast<int> (sc.chords.size()))
    {
        const auto& c = sc.chords[static_cast<size_t> (index)].chord;
        st->root = c.root;
        st->bass = c.bass;
        st->sharp = c.sharp;
        const auto& q = chords::qualitySuffixes();
        for (size_t i = 0; i < q.size(); ++i)
        {
            chords::Chord probe;
            if (chords::parseChord ("C" + q[i], probe) && probe.rel == c.rel) { st->quality = static_cast<int> (i); break; }
        }
    }
    const auto& suffixes = chords::qualitySuffixes();
    juce::StringArray quals;
    for (const auto& q : suffixes) quals.add (q.empty() ? juce::String::fromUTF8 ("(majeur)") : juce::String::fromUTF8 (q.c_str()));
    juce::StringArray basses;
    basses.add (juce::String::fromUTF8 ("— (aucune)"));
    basses.addArray (rootNames (st->sharp, naming));

    std::vector<Popup::Column> cols (3);
    cols[0].title = juce::String::fromUTF8 ("Fondamentale");
    cols[0].items = rootNames (st->sharp, naming);
    cols[0].selected = st->root;
    cols[1].title = juce::String::fromUTF8 ("Qualité");
    cols[1].items = quals;
    cols[1].selected = st->quality;
    cols[2].title = "Basse";
    cols[2].items = basses;
    cols[2].selected = st->bass + 1;
    popup_ = std::make_unique<Popup> (skin_, index >= 0 ? juce::String::fromUTF8 ("Changer l'accord") : juce::String::fromUTF8 ("Nouvel accord"), std::move (cols), true, index >= 0);
    auto* pop = popup_.get();
    auto build = [st, pop, suffixes, naming]
    {
        chords::Chord c;
        const auto q = suffixes[static_cast<size_t> (std::clamp (st->quality, 0, static_cast<int> (suffixes.size()) - 1))];
        const bool ok = chords::parseChord (std::string ("C") + q, c);
        c.root = st->root;
        c.bass = st->bass;
        c.sharp = st->sharp;
        if (! ok) c.rel = 0x091;
        pop->setText (juce::String::fromUTF8 (chords::formatChord (c, naming).c_str()));
        pop->setTextBad (false);
        return c;
    };
    auto fromColumns = [st, pop, build]
    {
        st->root = pop->selected (0);
        st->quality = pop->selected (1);
        st->bass = pop->selected (2) - 1;
        build();
    };
    pop->onChanged = fromColumns;
    pop->setExtra (st->sharp ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
    pop->onExtra = [st, pop, naming, fromColumns]
    {
        st->sharp = ! st->sharp;
        pop->setItems (0, rootNames (st->sharp, naming));
        juce::StringArray b;
        b.add (juce::String::fromUTF8 ("— (aucune)"));
        b.addArray (rootNames (st->sharp, naming));
        pop->setItems (2, b);
        pop->setExtra (st->sharp ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
        fromColumns();
    };
    pop->onTextChanged = [st, pop, suffixes] (const juce::String& t)
    {
        chords::Chord c;
        if (! chords::parseChord (t.toStdString(), c)) { pop->setTextBad (true); return; }
        pop->setTextBad (false);
        st->root = c.root;
        st->bass = c.bass;
        st->sharp = c.sharp;
        for (size_t i = 0; i < suffixes.size(); ++i)
        {
            chords::Chord probe;
            if (chords::parseChord ("C" + suffixes[i], probe) && probe.rel == c.rel) { st->quality = static_cast<int> (i); break; }
        }
        pop->setSelected (0, st->root);
        pop->setSelected (1, st->quality);
        pop->setSelected (2, st->bass + 1);
    };
    build();
    pop->onOk = [this, st, build, start, length, index] (const std::vector<int>&)
    {
        const auto c = build();
        closePopup();
        applyEdit (proc_, index >= 0 ? "Changer l'accord" : "Ajouter un accord", [&] (carte::Score& s)
        {
            if (index >= 0 && index < static_cast<int> (s.chords.size())) s.chords[static_cast<size_t> (index)].chord = c;
            else
            {
                chords::Item it;
                it.start = start;
                it.length = length;
                it.chord = c;
                chords::place (s.chords, it);
            }
        });
        view_->reload();
    };
    pop->onCancel = [this] { closePopup(); };
    pop->onDelete = [this, index]
    {
        closePopup();
        applyEdit (proc_, "Supprimer l'accord", [&] (carte::Score& s) { if (index >= 0 && index < static_cast<int> (s.chords.size())) chords::removeItem (s.chords, index); });
        view_->reload();
    };
    addAndMakeVisible (*popup_);
    popup_->setBounds (getLocalBounds());
    popup_->focusText();
}

void CarteEditor::showScalePicker (double start, double length, int index)
{
    closePopup();
    const auto naming = proc_.naming();
    const auto sc = proc_.score();
    int root = 0, mode = 0;
    bool sharp = false;
    if (index >= 0 && index < static_cast<int> (sc.scales.size()))
    {
        root = sc.scales[static_cast<size_t> (index)].root;
        mode = sc.scales[static_cast<size_t> (index)].mode;
        sharp = sc.scales[static_cast<size_t> (index)].sharp;
    }
    juce::StringArray modes;
    for (int i = 0; i < carte::scaleModeCount(); ++i)
        modes.add (juce::String::fromUTF8 (naming == chords::Naming::French ? carte::scaleMode (i).nameFr : carte::scaleMode (i).nameEn));
    std::vector<Popup::Column> cols (2);
    cols[0].title = juce::String::fromUTF8 ("Fondamentale");
    cols[0].items = rootNames (sharp, naming);
    cols[0].selected = root;
    cols[1].title = "Gamme";
    cols[1].items = modes;
    cols[1].selected = mode;
    popup_ = std::make_unique<Popup> (skin_, index >= 0 ? juce::String::fromUTF8 ("Changer la gamme") : juce::String::fromUTF8 ("Nouvelle gamme"), std::move (cols), false, index >= 0);
    auto* pop = popup_.get();
    auto flat = std::make_shared<bool> (sharp);
    pop->setExtra (sharp ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
    pop->onExtra = [pop, flat, naming]
    {
        *flat = ! *flat;
        pop->setItems (0, rootNames (*flat, naming));
        pop->setExtra (*flat ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
    };
    pop->onOk = [this, flat, start, length, index] (const std::vector<int>& sel)
    {
        carte::ScaleItem x;
        x.start = start;
        x.length = length;
        x.root = sel[0];
        x.mode = sel[1];
        x.sharp = *flat;
        closePopup();
        applyEdit (proc_, index >= 0 ? "Changer la gamme" : "Ajouter une gamme", [&] (carte::Score& s)
        {
            if (index >= 0 && index < static_cast<int> (s.scales.size())) { x.start = s.scales[static_cast<size_t> (index)].start; x.length = s.scales[static_cast<size_t> (index)].length; s.scales[static_cast<size_t> (index)] = x; }
            else carte::placeScale (s.scales, x);
        });
        view_->reload();
    };
    pop->onCancel = [this] { closePopup(); };
    pop->onDelete = [this, index]
    {
        closePopup();
        applyEdit (proc_, "Supprimer la gamme", [&] (carte::Score& s) { if (index >= 0 && index < static_cast<int> (s.scales.size())) s.scales.erase (s.scales.begin() + index); });
        view_->reload();
    };
    addAndMakeVisible (*popup_);
    popup_->setBounds (getLocalBounds());
    popup_->focusText();
}

void CarteEditor::showKeyPicker (double start, int index)
{
    closePopup();
    const auto naming = proc_.naming();
    const auto sc = proc_.score();
    int root = 0, minor = 0;
    bool sharp = false;
    if (index >= 0 && index < static_cast<int> (sc.keys.size()))
    {
        root = sc.keys[static_cast<size_t> (index)].root;
        minor = sc.keys[static_cast<size_t> (index)].minor ? 1 : 0;
        sharp = sc.keys[static_cast<size_t> (index)].sharp;
    }
    std::vector<Popup::Column> cols (2);
    cols[0].title = juce::String::fromUTF8 ("Fondamentale");
    cols[0].items = rootNames (sharp, naming);
    cols[0].selected = root;
    cols[1].title = "Mode";
    cols[1].items = { juce::String::fromUTF8 ("Majeur"), juce::String::fromUTF8 ("Mineur") };
    cols[1].selected = minor;
    popup_ = std::make_unique<Popup> (skin_, index >= 0 ? juce::String::fromUTF8 ("Changer la tonalité (jusqu'au marqueur suivant)") : juce::String::fromUTF8 ("Nouveau marqueur de tonalité"), std::move (cols), false, index >= 0);
    auto* pop = popup_.get();
    auto flat = std::make_shared<bool> (sharp);
    pop->setExtra (sharp ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
    pop->onExtra = [pop, flat, naming]
    {
        *flat = ! *flat;
        pop->setItems (0, rootNames (*flat, naming));
        pop->setExtra (*flat ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
    };
    pop->onOk = [this, flat, start, index] (const std::vector<int>& sel)
    {
        carte::KeyItem k;
        k.start = start;
        k.root = sel[0];
        k.minor = sel[1] == 1;
        k.sharp = *flat;
        closePopup();
        applyEdit (proc_, "Marqueur de tonalité", [&] (carte::Score& s)
        {
            if (index >= 0 && index < static_cast<int> (s.keys.size())) { k.start = s.keys[static_cast<size_t> (index)].start; s.keys[static_cast<size_t> (index)] = k; }
            else carte::placeKey (s.keys, k);
        });
        view_->reload();
    };
    pop->onCancel = [this] { closePopup(); };
    pop->onDelete = [this, index]
    {
        closePopup();
        applyEdit (proc_, "Supprimer le marqueur de tonalité", [&] (carte::Score& s) { if (index >= 0 && index < static_cast<int> (s.keys.size())) s.keys.erase (s.keys.begin() + index); });
        view_->reload();
    };
    addAndMakeVisible (*popup_);
    popup_->setBounds (getLocalBounds());
    popup_->focusText();
}

void CarteEditor::showPatternPicker (double start, double length, int index)
{
    closePopup();
    const auto sc = proc_.score();
    int slot = 1;
    if (index >= 0 && index < static_cast<int> (sc.patterns.size())) slot = sc.patterns[static_cast<size_t> (index)].slot;
    std::vector<Popup::Column> cols (1);
    cols[0].title = juce::String::fromUTF8 ("Emplacement de motif d'Arpège");
    for (int i = 1; i <= 16; ++i) cols[0].items.add (juce::String::fromUTF8 ("Motif ") + juce::String (i));
    cols[0].selected = slot - 1;
    popup_ = std::make_unique<Popup> (skin_, index >= 0 ? juce::String::fromUTF8 ("Changer le motif") : juce::String::fromUTF8 ("Placer un motif"), std::move (cols), false, index >= 0);
    auto* pop = popup_.get();
    pop->onOk = [this, start, length, index] (const std::vector<int>& sel)
    {
        carte::PatternItem x;
        x.start = start;
        x.length = length;
        x.slot = sel[0] + 1;
        closePopup();
        applyEdit (proc_, index >= 0 ? "Changer le motif" : "Placer un motif", [&] (carte::Score& s)
        {
            if (index >= 0 && index < static_cast<int> (s.patterns.size())) s.patterns[static_cast<size_t> (index)].slot = x.slot;
            else carte::placePattern (s.patterns, x);
        });
        view_->reload();
    };
    pop->onCancel = [this] { closePopup(); };
    pop->onDelete = [this, index]
    {
        closePopup();
        applyEdit (proc_, "Supprimer le motif", [&] (carte::Score& s) { if (index >= 0 && index < static_cast<int> (s.patterns.size())) s.patterns.erase (s.patterns.begin() + index); });
        view_->reload();
    };
    addAndMakeVisible (*popup_);
    popup_->setBounds (getLocalBounds());
    popup_->focusText();
}

void CarteEditor::showGlobalKeyPicker()
{
    closePopup();
    const auto naming = proc_.naming();
    const auto sc = proc_.score();
    std::vector<Popup::Column> cols (2);
    cols[0].title = juce::String::fromUTF8 ("Fondamentale");
    cols[0].items = rootNames (sc.hasGlobalKey && sc.globalKey.sharp, naming);
    cols[0].selected = sc.hasGlobalKey ? sc.globalKey.root : 0;
    cols[1].title = "Mode";
    cols[1].items = { juce::String::fromUTF8 ("Aucune tonalité globale"), juce::String::fromUTF8 ("Majeur"), juce::String::fromUTF8 ("Mineur") };
    cols[1].selected = ! sc.hasGlobalKey ? 0 : (sc.globalKey.minor ? 2 : 1);
    popup_ = std::make_unique<Popup> (skin_, juce::String::fromUTF8 ("Tonalité globale du morceau"), std::move (cols), false, false);
    auto* pop = popup_.get();
    auto flat = std::make_shared<bool> (sc.hasGlobalKey && sc.globalKey.sharp);
    pop->setExtra (*flat ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
    pop->onExtra = [pop, flat, naming]
    {
        *flat = ! *flat;
        pop->setItems (0, rootNames (*flat, naming));
        pop->setExtra (*flat ? juce::String::fromUTF8 ("Notes : dièses") : juce::String::fromUTF8 ("Notes : bémols"));
    };
    pop->onOk = [this, flat] (const std::vector<int>& sel)
    {
        closePopup();
        applyEdit (proc_, "Tonalité globale", [&] (carte::Score& s)
        {
            s.hasGlobalKey = sel[1] != 0;
            s.globalKey.root = sel[0];
            s.globalKey.minor = sel[1] == 2;
            s.globalKey.sharp = *flat;
        });
        view_->reload();
        refreshPills();
    };
    pop->onCancel = [this] { closePopup(); };
    addAndMakeVisible (*popup_);
    popup_->setBounds (getLocalBounds());
    popup_->focusText();
}

void CarteEditor::importFile()
{
    chooser_ = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Importer une piste d'accords (Scorsaire)"), juce::File(), "*.json");
    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        const auto t = proc_.transport();
        const double startBeats = t.hasPosition ? t.beats : 0.0;   // le morceau commence à la tête de lecture (0 sans hôte)
        const auto r = carte::importChordFile (f.loadFileAsString().toStdString(), startBeats);
        if (! r.ok)
        {
            hint_ = juce::String::fromUTF8 (r.error.c_str());
            repaint();
            return;
        }
        applyEdit (proc_, "Importer des accords", [&] (carte::Score& s)
        {
            s.chords = r.score.chords;      // les accords et les barres du fichier remplacent les anciens ; gammes, tonalités et motifs restent
            s.measures = r.score.measures;
        });
        view_->reload();
        refreshPills();
        hint_ = juce::String::fromUTF8 ("Importé : ") + juce::String (static_cast<int> (r.score.chords.size())) + " accords, " + juce::String (static_cast<int> (r.score.measures.size())) + " mesures"
                + (r.noChord > 0 ? juce::String::fromUTF8 (", ") + juce::String (r.noChord) + juce::String::fromUTF8 (" « pas d'accord » (trous)") : juce::String())
                + (r.unknownQualities > 0 ? juce::String::fromUTF8 (", ") + juce::String (r.unknownQualities) + juce::String::fromUTF8 (" qualité(s) inconnue(s) (fondamentale seule)") : juce::String())
                + juce::String::fromUTF8 (" à partir de ") + juce::String (startBeats, 2) + " temps";
        repaint();
    });
}

void CarteEditor::paint (juce::Graphics& g)
{
    g.fillAll (skin_.colour (theme::Token::BgApp));
    nova::paintHeaderBar (g, skin_, juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), skin_.px (46.0f)), "Carte", juce::String::fromUTF8 ("accords, gammes, tonalités, motifs"));
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
            text = juce::String::fromUTF8 ("Publiée sur le canal ") + juce::String (proc_.channel()) + juce::String::fromUTF8 (".  Double-clic : ajouter ou changer  ·  glisser : déplacer  ·  bord droit : durée  ·  rectangle : sélection  ·  Suppr  ·  Ctrl+C / V / D  ·  Ctrl+molette : zoom");
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
