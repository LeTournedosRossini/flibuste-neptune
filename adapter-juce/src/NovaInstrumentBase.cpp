#include "NovaInstrumentBase.h"

#include <algorithm>

using namespace neptune::core;
using neptune::core::theme::Token;

namespace
{
juce::Path matrixIcon()
{
    juce::Path p;
    for (const float x : { 4.0f, 14.0f }) for (const float y : { 4.0f, 14.0f }) p.addRectangle (x, y, 6.0f, 6.0f);
    return p;
}

juce::Path settingsIcon()
{
    juce::Path p;
    for (const float y : { 6.0f, 12.0f, 18.0f }) { p.startNewSubPath (3.0f, y); p.lineTo (21.0f, y); }
    p.addEllipse (14.0f, 4.0f, 4.0f, 4.0f);
    p.addEllipse (6.0f, 10.0f, 4.0f, 4.0f);
    p.addEllipse (12.0f, 16.0f, 4.0f, 4.0f);
    return p;
}
} // namespace

NovaInstrumentBase::NovaInstrumentBase (ParamHost& host, PresetHost& presets, InstrumentLink link, const theme::Palette& palette, theme::KnobStyle knobStyle,
                                        const i18n::Translator& tr, Options options, juce::String word)
    : EditorBase (host, palette, knobStyle, tr, std::move (options), std::move (word)), link_ (std::move (link)), presets_ (presets)
{
    updaters_.resize (16);
    ui_ = std::make_unique<nova::PresetUi> (*this, presets_, palette, tr, [this] { reloadDescription(); });
}

NovaInstrumentBase::~NovaInstrumentBase() = default;

// ------------------------------------------------------------------------------------------------ fabriques
void NovaInstrumentBase::onUpdate (int page, std::function<void()> f) { updaters_[static_cast<std::size_t> (page + 1)].push_back (std::move (f)); }
void NovaInstrumentBase::gestureBegin() { dragging_ = true; if (link_.beginGesture) link_.beginGesture(); }
void NovaInstrumentBase::gestureEnd() { if (link_.endGesture) link_.endGesture(); dragging_ = false; }

nova::ValueSlider* NovaInstrumentBase::sliderFn (int page, juce::String title, double lo, double hi, double step, juce::String suffix, std::function<double()> get,
                                                 std::function<void (double)> set, double def, std::function<bool()> enabled)
{
    auto* s = add<nova::ValueSlider> (page, skin(), std::move (title), lo, hi, step, std::move (suffix), get, std::move (set), def);
    s->gestureBegin = [this] { gestureBegin(); };
    s->gestureEnd = [this] { gestureEnd(); };
    onUpdate (page, [s, enabled] { s->update(); if (enabled) s->setActive (enabled()); });
    return s;
}

nova::NumberPill* NovaInstrumentBase::numberFn (int page, juce::String label, int lo, int hi, std::function<int()> get, std::function<void (int)> set,
                                                std::function<bool()> enabled, std::function<juce::String (int)> text)
{
    auto* n = add<nova::NumberPill> (page, skin(), std::move (label), lo, hi, std::move (get), std::move (set), std::move (text));
    n->gestureBegin = [this] { gestureBegin(); };
    n->gestureEnd = [this] { gestureEnd(); };
    onUpdate (page, [n, enabled] { n->update(); if (enabled) n->setActive (enabled()); });
    return n;
}

nova::ChoicePill* NovaInstrumentBase::choiceFn (int page, juce::String label, std::vector<juce::String> items, std::function<int()> get, std::function<void (int)> set,
                                                std::function<bool()> enabled)
{
    auto c = std::make_unique<nova::ChoicePill>();
    c->pill = add<nova::Pill> (page, skin(), nova::Pill::Look::Small);
    c->label = [label] { return label; };
    c->items = std::move (items);
    c->get = std::move (get);
    c->set = [this, set] (int v) { gestureBegin(); set (v); gestureEnd(); };
    c->attach (*this);
    auto* raw = c.get();
    choices_.push_back (std::move (c));
    onUpdate (page, [raw, enabled] { raw->refresh (! enabled || enabled()); });
    return raw;
}

nova::Pill* NovaInstrumentBase::toggleFn (int page, juce::String text, std::function<bool()> get, std::function<void (bool)> set, std::function<bool()> enabled)
{
    auto* p = add<nova::Pill> (page, skin(), nova::Pill::Look::Small);
    p->setText (text);
    p->onClick = [this, get, set] { const bool now = ! get(); gestureBegin(); set (now); gestureEnd(); };
    onUpdate (page, [p, get, enabled] { p->setOn (get()); const bool en = ! enabled || enabled(); p->setAlpha (en ? 1.0f : 0.4f); p->setInterceptsMouseClicks (en, en); });
    return p;
}

juce::String NovaInstrumentBase::paramTitle (ParamId id)
{
    const auto& set = host().parameters();
    const auto index = set.indexOf (id);
    return index == ParameterSet::npos ? juce::String() : juce::String::fromUTF8 (set.spec (index).name.c_str()).toUpperCase();
}

nova::KnobCard* NovaInstrumentBase::knob (int page, ParamId id, juce::String title, bool compact)
{
    auto* k = add<nova::KnobCard> (page, skin(), host(), id, title.isEmpty() ? paramTitle (id) : title, modalOpener());
    k->setPlain (true);
    k->setCompact (compact);
    return k;
}

// ------------------------------------------------------------------------------------------------ construction commune
void NovaInstrumentBase::buildCommon (const char* outputHelpKey)
{
    addPage (tx ("og.page_matrix"), matrixIcon());
    addPage (tx ("nv.page_settings"), settingsIcon());

    // colonne de gauche
    outPanel_ = add<nova::Panel> (-1, skin());
    outPanel_->setTitle (tx ("sy.output").toUpperCase());
    navPanel_ = add<nova::Panel> (-1, skin());
    navPanel_->toBack();   // sous les entrées du menu, créées avant lui
    for (const auto id : link_.outputs) outKnobs_.push_back (knob (-1, id, {}, true));
    kbPill_ = add<nova::Pill> (-1, skin(), nova::Pill::Look::Small);
    kbPill_->onClick = [this] { keyboardShown_ = ! keyboardShown_; if (! keyboardShown_) keyboard_->releaseAll(); applyKeyboard(); resized(); };
    if (link_.carteMenu)
    {
        cartePill_ = add<nova::Pill> (-1, skin());
        cartePill_->onClick = [this] { link_.carteMenu (cartePill_); };
    }
    addInfo (-1, outPanel_, outputHelpKey, 1);

    // page MATRICE
    const int mp = matrixPage();
    matrixPanel_ = add<nova::Panel> (mp, skin());
    matrixPanel_->setTitle (tx ("og.matrix_title").toUpperCase());
    tiles_ = add<nova::PresetTiles> (mp, skin(), presets_);
    tiles_->onLoaded = [this] { reloadDescription(); };
    tiles_->textLoad = tx ("sy.tile_load");
    tiles_->textAssign = tx ("sy.tile_assign");
    tiles_->textClear = tx ("sy.tile_clear");
    addInfo (mp, matrixPanel_, "hlp.sy_matrix", true);
    bankPrev_ = add<nova::Pill> (mp, skin(), nova::Pill::Look::Small);
    bankPrev_->setText ("<");
    bankPrev_->onClick = [this] { tiles_->bank = (tiles_->bank + preset::numBanks - 1) % preset::numBanks; };
    bankNext_ = add<nova::Pill> (mp, skin(), nova::Pill::Look::Small);
    bankNext_->setText (">");
    bankNext_->onClick = [this] { tiles_->bank = (tiles_->bank + 1) % preset::numBanks; };
    bankPill_ = add<nova::Pill> (mp, skin());
    bankPill_->setStatic (true);
    colsPill_ = add<nova::Pill> (mp, skin());
    colsPill_->onClick = [this] { tiles_->cols = tiles_->cols % 8 + 1; };
    rowsPill_ = add<nova::Pill> (mp, skin());
    rowsPill_->onClick = [this] { tiles_->rows = tiles_->rows % 8 + 1; };
    fieldPill_ = add<nova::Pill> (mp, skin());
    fieldPill_->onClick = [this] { tiles_->field = (tiles_->field + 1) % 5; };
    addInfo (mp, fieldPill_, "hlp.og_field");
    fieldsTitle_ = add<nova::Text> (mp, skin(), Token::TextDim2, 11.0f, true);
    fieldsTitle_->setText (tx ("sy.fields_title").toUpperCase());
    fields_ = add<nova::PresetFields> (mp, skin(), presets_, translator());
    addInfo (mp, fieldsTitle_, "hlp.sy_fields");
    fillPill_ = add<nova::Pill> (mp, skin());
    fillPill_->setText (tx ("sy.fill_factory"));
    fillPill_->onClick = [this] { fillBankWithFactoryPresets(); };
    addInfo (mp, fillPill_, "hlp.sy_fill");

    // clavier (toutes les pages sauf SETTINGS)
    keyboardPanel_ = add<nova::Panel> (-2, skin());
    keyboardPanel_->setTitle (tx ("synth.keyboard").toUpperCase());
    addInfo (-2, keyboardPanel_, "hlp.sy_keyboard", 1);
    keyboard_ = add<SynthKeyboard> (-2, skin().palette());
    keyboard_->onNoteOn = [this] (int key, float v) { if (link_.noteOn) link_.noteOn (key, v); };
    keyboard_->onNoteOff = [this] (int key) { if (link_.noteOff) link_.noteOff (key); };
    if (link_.pitchBend || link_.modWheel)
    {
        bendWheel_ = add<KeyboardWheel> (-2, skin().palette(), true, "PITCH");
        bendWheel_->onChange = [this] (float v) { if (link_.pitchBend) link_.pitchBend (v); };
        modWheel_ = add<KeyboardWheel> (-2, skin().palette(), false, "MOD");
        modWheel_->onChange = [this] (float v) { if (link_.modWheel) link_.modWheel (v); };
    }
    octDown_ = add<nova::Pill> (-2, skin(), nova::Pill::Look::Small);
    octDown_->setText (tx ("synth.oct_down"));
    octDown_->onClick = [this] { keyboard_->shiftOctave (-1); };
    octUp_ = add<nova::Pill> (-2, skin(), nova::Pill::Look::Small);
    octUp_->setText (tx ("synth.oct_up"));
    octUp_->onClick = [this] { keyboard_->shiftOctave (1); };
    holdPill_ = add<nova::Pill> (-2, skin(), nova::Pill::Look::Small);
    holdPill_->setText (tx ("synth.hold"));
    holdPill_->onClick = [this] { holdPill_->setOn (! holdPill_->isOn()); keyboard_->setHold (holdPill_->isOn()); };
    panicPill_ = add<nova::Pill> (-2, skin(), nova::Pill::Look::Small);
    panicPill_->setText (tx ("panic.button"));
    panicPill_->onClick = [this] { if (link_.panic) link_.panic(); keyboard_->releaseAll(); };

    buildSettingsPage (settingsPage());
}

// ------------------------------------------------------------------------------------------------ presets
// Place les presets d'usine acceptés par l'instrument dans les emplacements libres de la banque affichée (ceux déjà présents sont laissés).
void NovaInstrumentBase::fillBankWithFactoryPresets()
{
    auto& lib = presets_.library();
    const int bank = tiles_->bank;
    std::vector<std::string> present;
    for (int prog = 0; prog < preset::programsPerBank; ++prog)
    {
        const auto id = preset::Navigator::presetAt (lib.banks(), { bank, prog });
        if (! id.empty()) present.push_back (id);
    }
    int prog = 0, placed = 0;
    for (const auto& p : lib.factoryPresets())
    {
        if (! presets_.acceptsPreset (p) || std::find (present.begin(), present.end(), p.id) != present.end()) continue;
        while (prog < preset::programsPerBank && ! preset::Navigator::presetAt (lib.banks(), { bank, prog }).empty()) ++prog;
        if (prog >= preset::programsPerBank) break;
        if (lib.assign (bank, prog, p.id)) ++placed;
    }
    presets_.touchPresets();
    ui_->statusText = juce::String (placed) + " " + tx ("sy.fill_done");
}

void NovaInstrumentBase::showPresetMenu (juce::Component* anchor) { ui_->showMenu (anchor, tiles_ != nullptr ? tiles_->bank : 0); }

bool NovaInstrumentBase::keyPressed (const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    if (m.isCommandDown() && (k.getKeyCode() == 'S' || k.getKeyCode() == 's')) { ui_->quickSave(); return true; }
    return EditorBase::keyPressed (k);
}

// ------------------------------------------------------------------------------------------------ clavier masquable
void NovaInstrumentBase::applyKeyboard()
{
    const bool shown = keyboardShown_ && currentPage() != settingsPage();
    for (juce::Component* c : std::initializer_list<juce::Component*> { keyboardPanel_, keyboard_, octDown_, octUp_, holdPill_, panicPill_, bendWheel_, modWheel_ })
        if (c != nullptr) c->setVisible (shown);
    setInfoVisible (keyboardPanel_, shown);
}

void NovaInstrumentBase::pageChanged() { if (keyboardPanel_ != nullptr) applyKeyboard(); }

// ------------------------------------------------------------------------------------------------ mise en page commune
void NovaInstrumentBase::layoutCommon()
{
    const float H = contentH();

    // ---- colonne de gauche
    outPanel_->setBounds (R (14, 72, 176, 316));
    for (std::size_t i = 0; i < outKnobs_.size(); ++i)
        outKnobs_[i]->setBounds (R (22.0f + static_cast<float> (i % 2) * 84.0f, 106.0f + static_cast<float> (i / 2) * 92.0f, 80, 88));
    navPanel_->setBounds (R (14, 400, 176, 310));
    layoutNav (400, 250);
    kbPill_->setText (tx (keyboardShown_ ? "synth.kb_shown" : "synth.kb_hidden"));
    kbPill_->setBounds (R (24, 664, 156, 30));

    // ---- page MATRICE
    matrixPanel_->setBounds (R (X0, 72, 942, H));
    bankPrev_->setBounds (R (X0 + 16, 72 + 36, 36, 30));
    bankPill_->setBounds (R (X0 + 56, 72 + 36, 150, 30));
    bankNext_->setBounds (R (X0 + 210, 72 + 36, 36, 30));
    colsPill_->setBounds (R (X0 + 266, 72 + 36, 150, 30));
    rowsPill_->setBounds (R (X0 + 426, 72 + 36, 150, 30));
    fieldPill_->setBounds (R (X0 + 586, 72 + 36, 190, 30));
    fillPill_->setBounds (R (X0 + 784, 72 + 36, 124, 30));
    tiles_->setBounds (R (X0 + 16, 72 + 80, 640, H - 92));
    fieldsTitle_->setBounds (R (X0 + 676, 72 + 80, 240, 16));
    fields_->setBounds (R (X0 + 676, 72 + 100, 250, H - 112));

    // ---- clavier
    keyboardPanel_->setBounds (R (X0, 596, 942, 114));
    if (bendWheel_ != nullptr)   // deux molettes à gauche : le clavier perd leur largeur
    {
        bendWheel_->setBounds (R (X0 + 12, 622, 34, 86));
        modWheel_->setBounds (R (X0 + 52, 622, 34, 86));
        keyboard_->setBounds (R (X0 + 94, 626, 618, 78));
    }
    else
        keyboard_->setBounds (R (X0 + 12, 626, 700, 78));
    octDown_->setBounds (R (X0 + 732, 628, 60, 26));
    octUp_->setBounds (R (X0 + 798, 628, 60, 26));
    holdPill_->setBounds (R (X0 + 864, 628, 66, 26));
    panicPill_->setBounds (R (X0 + 732, 664, 198, 30));
    applyKeyboard();

    // ---- barre du bas, en-tête, gestionnaire
    abPill()->setBounds (R (14, 724 + 12, 40, 32));
    if (cartePill_ != nullptr) cartePill_->setBounds (R (64, 724 + 12, 380, 32));
    presetPill()->setBounds (R (440, 11, 280, 36));
    ui_->resized();
}

void NovaInstrumentBase::tickCommon()
{
    presetPill()->setText (ui_->label());
    if (cartePill_ != nullptr && link_.carteText)
    {
        const auto t = link_.carteText();
        if (t != carteShown_) { carteShown_ = t; cartePill_->setText (t); }
        cartePill_->setOn (t.startsWith (juce::String::fromUTF8 ("Carte : ON")));
    }
    for (auto& f : updaters_[0]) f();
    const int page = currentPage();
    if (page >= 0 && page + 1 < static_cast<int> (updaters_.size())) for (auto& f : updaters_[static_cast<std::size_t> (page + 1)]) f();
    if (keyboardShown_ && page != settingsPage() && link_.heldKeys) keyboard_->setHeldKeys (link_.heldKeys (0), link_.heldKeys (1));
    if (page == matrixPage())
    {
        tiles_->update();
        fields_->update();
        bankPill_->setText (tx ("og.bank") + " " + juce::String::fromUTF8 (preset::defaultBankName (tiles_->bank).c_str()) + " (" + juce::String (tiles_->bank) + ")");
        colsPill_->setText (tx ("og.columns") + "  " + juce::String (tiles_->cols));
        rowsPill_->setText (tx ("og.rows") + "  " + juce::String (tiles_->rows));
        static const char* const fields[5] = { "og.f_preset", "og.f_song", "og.f_workshop", "og.f_student", "og.f_event" };
        fieldPill_->setText (tx ("og.main_field") + ": " + tx (fields[tiles_->field]));
    }
}

void NovaInstrumentBase::paintStatus (juce::Graphics& g)
{
    g.setColour (skin().colour (Token::TextDim2));
    g.setFont (skin().font (11.0f, true));
    if (ui_->statusText.isNotEmpty()) g.drawText (ui_->statusText, R (730, 724 + 12, 416, 32).toFloat(), juce::Justification::centredRight);
}
