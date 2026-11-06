#include "NovaEditor.h"
#include "ReserveOverlay.h"
#include "neptune/core/Version.h"

#include <cmath>

#include <nlohmann/json.hpp>
#include "neptune/core/StockModule.h"

using namespace neptune::core;
using neptune::core::theme::Token;

namespace nova
{
EditorBase::EditorBase (ParamHost& host, const theme::Palette& palette, theme::KnobStyle knob, const i18n::Translator& tr, Options options, juce::String word)
    : host_ (host), skin_ (palette, knob), look_ (skin_), tr_ (tr), options_ (std::move (options)), word_ (std::move (word))
{
    setWantsKeyboardFocus (true);
    setLookAndFeel (&look_);

    preset_ = add<Pill> (-1, skin_);
    preset_->setText (juce::String::fromUTF8 ("--"));
    preset_->onClick = [this] { if (options_.presetMenu) options_.presetMenu (preset_); else showPresetMenu(); };

    ab_ = add<Pill> (-1, skin_, Pill::Look::Small);
    ab_->setText ("A");
    ab_->setFontSize (18.0f);
    ab_->onClick = [this] { toggleAB(); };

    if (options_.brand && ! options_.noBypass)
    {
        bypass_ = add<Pill> (-1, skin_, Pill::Look::Icon);
        juce::Path power;
        power.addCentredArc (12.0f, 13.0f, 7.5f, 7.5f, 0.0f, 0.65f, juce::MathConstants<float>::twoPi - 0.65f, true);
        power.startNewSubPath (12.0f, 3.0f);
        power.lineTo (12.0f, 11.0f);
        bypass_->setIcon (power);
        bypass_->setTintToken (Token::KnobHot);
        bypass_->bindToggle (host_, stock::bypass, {});
        bypass_->setText ({});
    }
    if (options_.brand && options_.getSettings && options_.applySettings)
    {
        hdrTheme_ = add<Pill> (-1, skin_, Pill::Look::Small);
        hdrTheme_->setFontSize (10.0f);
        hdrTheme_->onClick = [this]
        {
            auto st = options_.getSettings();
            static const char* const ids[4] = { "dark", "light", "hc", "hammond" };
            int cur = 0;
            for (int i = 0; i < 4; ++i) if (st.theme == ids[i]) cur = i;
            st.theme = ids[(cur + 1) % 4];
            options_.applySettings (st);
        };
        hdrMinus_ = add<Pill> (-1, skin_, Pill::Look::Small);
        hdrMinus_->setText (juce::String::fromUTF8 ("\xe2\x88\x92"));
        hdrMinus_->setFontSize (16.0f);
        hdrMinus_->onClick = [this] { auto st = options_.getSettings(); if (st.contrast > 0) { --st.contrast; options_.applySettings (st); } };
        hdrContrast_ = add<Pill> (-1, skin_, Pill::Look::Small);
        hdrContrast_->setStatic (true);
        hdrContrast_->setFontSize (12.0f);
        hdrPlus_ = add<Pill> (-1, skin_, Pill::Look::Small);
        hdrPlus_->setText ("+");
        hdrPlus_->setFontSize (16.0f);
        hdrPlus_->onClick = [this] { auto st = options_.getSettings(); if (st.contrast < 4) { ++st.contrast; options_.applySettings (st); } };
        refreshSettingsTexts();
    }
    if (options_.brand && options_.reserveOverlay && host_.reserveCount() > 0)
    {
        hdrAuto_ = add<Pill> (-1, skin_, Pill::Look::Small);
        hdrAuto_->setFontSize (10.0f);
        hdrAuto_->setText (juce::String::fromUTF8 ("AUTOMATION"));
        hdrAuto_->onClick = [this] { if (reserveOverlay_ != nullptr) { reserveOverlay_->setVisible (true); reserveOverlay_->toFront (true); } };
        reserveOverlay_ = std::make_unique<ReserveOverlay> (skin_, host_, [this] { reserveOverlay_->setVisible (false); });
        addChildComponent (*reserveOverlay_);
    }
    startTimerHz (30);
}

EditorBase::~EditorBase()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void EditorBase::addChild (juce::Component* c, int page)
{
    addAndMakeVisible (c);
    items_.push_back ({ c, page });
    if (auto* r = dynamic_cast<Refreshable*> (c)) refreshables_.push_back (r);
}

InfoButton* EditorBase::addInfo (int page, juce::Component* target, const char* key, int pos)
{
    auto* b = add<InfoButton> (page, skin_, tx ((std::string (key) + ".t").c_str()), tx (key));
    infos_.push_back ({ b, target, pos });
    return b;
}

void EditorBase::setInfoVisible (juce::Component* target, bool visible)
{
    for (auto& i : infos_)
        if (i.target == target) i.button->setVisible (visible);
}

void EditorBase::setInfoText (juce::Component* target, const char* key)
{
    for (auto& i : infos_)
        if (i.target == target) i.button->setTexts (tx ((std::string (key) + ".t").c_str()), tx (key));
}

void EditorBase::placeInfos()
{
    const int d = skin_.ipx (16.0f);
    for (auto& i : infos_)
    {
        const auto t = i.target->getBounds();
        if (i.pos == 1) i.button->setBounds (t.getRight() - d - skin_.ipx (6.0f), t.getY() + skin_.ipx (6.0f), d, d);
        else if (i.pos == 2) i.button->setBounds (t.getCentreX() - d / 2, t.getBottom() + skin_.ipx (2.0f), d, d);
        else i.button->setBounds (t.getRight() + skin_.ipx (4.0f), t.getCentreY() - d / 2, d, d);
        i.button->toFront (false);
    }
}

int EditorBase::addPage (juce::String label, juce::Path icon)
{
    const int index = static_cast<int> (navs_.size());
    auto* n = add<NavItem> (-1, skin_, std::move (label), std::move (icon));
    n->setActive (index == 0);
    n->onClick = [this, index] { setPage (index); };
    navs_.push_back (n);
    return index;
}

void EditorBase::setPage (int p)
{
    page_ = juce::jlimit (0, juce::jmax (0, numPages() - 1), p);
    for (std::size_t i = 0; i < navs_.size(); ++i) navs_[i]->setActive (static_cast<int> (i) == page_);
    for (auto& it : items_) it.c->setVisible (it.page == -1 || it.page == page_ || (it.page == -2 && page_ != settingsPage_));
    if (options_.noPresets && preset_ != nullptr) preset_->setVisible (false);
    if (options_.noAB && ab_ != nullptr) ab_->setVisible (false);
    if (modal_ != nullptr) modal_->setVisible (true);
    pageChanged();
    repaint();
}

void EditorBase::resized()
{
    const float s = fitScale (getWidth(), getHeight(), designWidth, designHeight, minScale, maxScale);
    skin_.setScale (s);
    frame_ = fitRect (getWidth(), getHeight(), designWidth, designHeight, s);
    extraW_ = 0.0f;
    if (options_.stretchWidth || options_.stretchAuto)
    {
        // la hauteur fixe l'échelle ; toute la largeur de la fenêtre est utilisée, le surplus (en pixels de maquette) va à la zone de droite
        frame_ = { 0, frame_.getY(), getWidth(), frame_.getHeight() };
        extraW_ = std::max (0.0f, static_cast<float> (getWidth()) / s - static_cast<float> (designWidth));
    }
    layout();
    layoutSettings();
    layoutHeaderControls();
    placeInfos();
    if (modal_ != nullptr) modal_->setBounds (getLocalBounds());
    repaint();
}

void EditorBase::paint (juce::Graphics& g)
{
    g.fillAll (skin_.colour (Token::BgApp));
    const auto f = frame_.toFloat();
    paintHeaderBar (g, skin_, f.withHeight (skin_.px (headerH)), word_, tx ("nv.brand_sub"), options_.embedded ? HeaderStyle::Compact : HeaderStyle::Full);
    paintBottomBar (g, skin_, f.withTop (f.getBottom() - skin_.px (bottomH)), options_.embedded ? juce::String() : juce::String (neptune::core::version::kString));
    paintBackground (g);
}

void EditorBase::timerCallback()
{
    for (auto* r : refreshables_) r->refresh();
    if (hdrTheme_ != nullptr || (settingsPage_ >= 0 && page_ == settingsPage_)) refreshSettingsTexts();
    tick();
}

void EditorBase::openModal (ParamId id, uistate::Terminal terminal)
{
    if (modal_) return;
    modal_ = std::make_unique<KnobModal> (host_, id, terminal, skin_.palette(), tr_,
                                          [safe = juce::Component::SafePointer<EditorBase> (this)]
                                          {
                                              juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeModal(); });
                                          });
    addAndMakeVisible (*modal_);
    modal_->setBounds (getLocalBounds());
    modal_->toFront (true);
}

void EditorBase::closeModal()
{
    if (! modal_) return;
    host_.setModalSize (modal_->terminal(), modal_->panelSize());
    modal_.reset();
    grabKeyboardFocus();
}

bool EditorBase::keyPressed (const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    if (! m.isCommandDown()) return false;
    const int code = k.getKeyCode();
    if (code == 'Z' || code == 'z') { if (m.isShiftDown()) host_.uiRedo(); else host_.uiUndo(); return true; }
    if (code == 'Y' || code == 'y') { host_.uiRedo(); return true; }
    return false;
}

std::map<ParamId, double> EditorBase::snapshot() const
{
    std::map<ParamId, double> m;
    const auto& set = host_.parameters();
    for (std::size_t i = 0; i < set.size(); ++i)
        if (! set.spec (i).hidden) m[set.spec (i).id] = set.getAt (i);   // les réglages d'affichage cachés ne font pas partie du A/B
    return m;
}

void EditorBase::restore (const std::map<ParamId, double>& m)
{
    for (const auto& [id, v] : m)
        if (value (id) != v) setParam (host_, id, v);
}

void EditorBase::toggleAB()
{
    // A/B : deux jeux complets ; on bascule en gardant l'autre dans l'éditeur (non enregistré dans le projet)
    auto current = snapshot();
    if (otherSlot_.empty()) otherSlot_ = current;
    restore (otherSlot_);
    otherSlot_ = std::move (current);
    slotB_ = ! slotB_;
    ab_->setText (slotB_ ? "B" : "A");
}

// ---- menu latéral : entrées centrées dans leur zone
void EditorBase::layoutNav (float zoneY, float zoneH)
{
    const int n = numPages();
    if (n == 0) return;
    const float gap = 4.0f, itemH = std::min (44.0f, (zoneH - 20.0f - gap * static_cast<float> (n - 1)) / static_cast<float> (n));
    const float total = itemH * static_cast<float> (n) + gap * static_cast<float> (n - 1), top = zoneY + (zoneH - total) / 2.0f;
    for (int i = 0; i < n; ++i) nav (i)->setBounds (R (14.0f + 14.0f, top + (itemH + gap) * static_cast<float> (i), 148.0f, itemH));
}

// ---- presets utilisateur : un fichier JSON par preset dans <configuration de la suite>/Presets/<module>
juce::File EditorBase::userPresetsDir() const
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Neptune").getChildFile ("Presets").getChildFile (options_.moduleName.isEmpty() ? juce::String ("module") : options_.moduleName);
}

void EditorBase::saveUserPreset (const juce::String& rawName)
{
    auto name = rawName.trim();
    if (name.isEmpty()) return;
    auto fileName = name.replaceCharacters ("\\/:*?\"<>|", "_________");
    const auto dir = userPresetsDir();
    if (! dir.createDirectory().wasOk()) return;
    nlohmann::json doc;
    doc["format"] = "nova-preset";
    doc["version"] = 1;
    doc["module"] = options_.moduleName.toStdString();
    doc["name"] = name.toStdString();
    nlohmann::json params = nlohmann::json::object();
    const auto& set = host_.parameters();
    for (std::size_t i = 0; i < set.size(); ++i)
    {
        const auto& spec = set.spec (i);
        if (spec.hidden || spec.id == stock::bypass) continue;   // les réglages d'affichage et le contournement ne font pas partie d'un preset
        params[std::to_string (spec.id)] = set.getAt (i);
    }
    doc["parameters"] = std::move (params);
    dir.getChildFile (fileName + ".json").replaceWithText (juce::String::fromUTF8 (doc.dump (2).c_str()));
    preset_->setText (name);
}

bool EditorBase::loadUserPreset (const juce::File& file)
{
    const auto text = file.loadFileAsString().toStdString();
    const auto doc = nlohmann::json::parse (text, nullptr, false);
    if (doc.is_discarded() || ! doc.is_object() || doc.value ("format", std::string()) != "nova-preset") return false;
    const auto params = doc.find ("parameters");
    if (params == doc.end() || ! params->is_object()) return false;
    for (const auto& [key, v] : params->items())
    {
        if (! v.is_number()) continue;
        const long long id = std::strtoll (key.c_str(), nullptr, 10);
        const auto* spec = host_.parameters().find (static_cast<ParamId> (id));
        if (spec == nullptr || spec->hidden || spec->id == stock::bypass) continue;
        const double plain = spec->clamp (v.get<double>());
        if (value (spec->id) != plain) setParam (host_, spec->id, plain);   // un geste par paramètre modifié
    }
    preset_->setText (juce::String::fromUTF8 (doc.value ("name", file.getFileNameWithoutExtension().toStdString()).c_str()));
    return true;
}

namespace
{
// Petite fenêtre de saisie d'un nom, superposée à l'éditeur
class NameEntry : public juce::Component
{
public:
    NameEntry (const Skin& s, const juce::String& title, const juce::String& okText, const juce::String& cancelText, std::function<void (juce::String)> done, std::function<void()> close)
        : skin_ (s), title_ (title), done_ (std::move (done)), close_ (std::move (close)), ok_ (s), cancel_ (s)
    {
        editor_.setMultiLine (false);
        editor_.setColour (juce::TextEditor::backgroundColourId, s.colour (Token::BgCard));
        editor_.setColour (juce::TextEditor::textColourId, s.colour (Token::Text));
        editor_.setColour (juce::TextEditor::outlineColourId, s.colour (Token::CtlBd));
        editor_.setColour (juce::TextEditor::focusedOutlineColourId, s.colour (Token::Accent));
        editor_.setFont (juce::FontOptions (18.0f));
        editor_.onReturnKey = [this] { accept(); };
        editor_.onEscapeKey = [this] { if (close_) close_(); };
        addAndMakeVisible (editor_);
        ok_.setText (okText);
        ok_.onClick = [this] { accept(); };
        cancel_.setText (cancelText);
        cancel_.onClick = [this] { if (close_) close_(); };
        addAndMakeVisible (ok_);
        addAndMakeVisible (cancel_);
        setWantsKeyboardFocus (true);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f));
        g.setColour (skin_.colour (Token::BgCard));
        g.fillRoundedRectangle (panel_.toFloat(), 10.0f);
        g.setColour (skin_.colour (Token::Border));
        g.drawRoundedRectangle (panel_.toFloat().reduced (0.5f), 10.0f, 1.0f);
        g.setColour (skin_.colour (Token::Text));
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText (title_, panel_.getX() + 16, panel_.getY() + 12, panel_.getWidth() - 32, 24, juce::Justification::centredLeft);
    }
    void resized() override
    {
        panel_ = juce::Rectangle<int> (420, 170).withCentre (getLocalBounds().getCentre());
        auto r = panel_.reduced (16);
        r.removeFromTop (34);
        editor_.setBounds (r.removeFromTop (44));
        r.removeFromTop (14);
        auto b = r.removeFromTop (40);
        cancel_.setBounds (b.removeFromRight (120));
        b.removeFromRight (8);
        ok_.setBounds (b.removeFromRight (120));
    }
    void mouseDown (const juce::MouseEvent& e) override { if (! panel_.contains (e.getPosition()) && close_) close_(); }
    void focus() { editor_.grabKeyboardFocus(); }

private:
    void accept()
    {
        const auto text = editor_.getText().trim();
        if (text.isEmpty()) return;
        auto done = done_;
        if (close_) close_();
        if (done) done (text);
    }
    Skin skin_;
    juce::String title_;
    std::function<void (juce::String)> done_;
    std::function<void()> close_;
    juce::TextEditor editor_;
    Pill ok_, cancel_;
    juce::Rectangle<int> panel_;
};
} // namespace

void EditorBase::askName (const juce::String& title, std::function<void (juce::String)> done)
{
    if (nameEntry_) return;
    auto entry = std::make_unique<NameEntry> (skin_, title, tx ("nv.ok"), tx ("nv.cancel"), std::move (done),
                                              [safe = juce::Component::SafePointer<EditorBase> (this)]
                                              {
                                                  juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->nameEntry_.reset(); safe->grabKeyboardFocus(); } });
                                              });
    addAndMakeVisible (*entry);
    entry->setBounds (getLocalBounds());
    entry->toFront (true);
    auto* raw = entry.get();
    nameEntry_ = std::move (entry);
    raw->focus();
}

void EditorBase::showPresetMenu()
{
    const auto names = presetNames();
    const auto files = userPresetsDir().findChildFiles (juce::File::findFiles, false, "*.json");
    juce::PopupMenu menu;
    for (std::size_t i = 0; i < names.size(); ++i) menu.addItem (static_cast<int> (i) + 1, names[i], true, static_cast<int> (i) == presetIndex_);
    juce::Array<juce::File> sorted (files);
    sorted.sort();
    if (! sorted.isEmpty())
    {
        menu.addSeparator();
        menu.addSectionHeader (tx ("nv.pr_user"));
        for (int i = 0; i < sorted.size(); ++i) menu.addItem (1000 + i, sorted[i].getFileNameWithoutExtension());
    }
    menu.addSeparator();
    menu.addItem (900, tx ("nv.pr_save"));
    if (! sorted.isEmpty())
    {
        juce::PopupMenu del;
        for (int i = 0; i < sorted.size(); ++i) del.addItem (2000 + i, sorted[i].getFileNameWithoutExtension());
        menu.addSubMenu (tx ("nv.pr_delete"), del);
    }
    menu.addItem (901, tx ("nv.pr_folder"));
    menu.setLookAndFeel (&getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (preset_), [safe = juce::Component::SafePointer<EditorBase> (this), names, sorted] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        if (choice == 900) { safe->askName (safe->tx ("nv.pr_name_title"), [safe] (juce::String name) { if (safe != nullptr) safe->saveUserPreset (name); }); return; }
        if (choice == 901) { auto dir = safe->userPresetsDir(); dir.createDirectory(); dir.revealToUser(); return; }
        if (choice >= 2000) { sorted[choice - 2000].deleteFile(); return; }
        if (choice >= 1000) { safe->loadUserPreset (sorted[choice - 1000]); return; }
        safe->presetIndex_ = choice - 1;
        const auto values = safe->presetValues (choice - 1);
        // un geste par paramètre modifié (annulable, visible de l'hôte)
        for (const auto id : safe->presetParamIds())
        {
            const auto* spec = safe->host_.parameters().find (id);
            if (spec == nullptr) continue;
            double v = spec->defaultValue;
            for (const auto& [pid, val] : values) if (pid == id) v = val;
            if (safe->value (id) != v) setParam (safe->host_, id, v);
        }
        safe->preset_->setText (names[static_cast<std::size_t> (choice - 1)]);
    });
}

// ---- page SETTINGS
void EditorBase::buildSettingsPage (int page)
{
    settingsPage_ = page;
    setPanel_ = add<Panel> (page, skin_);
    setPanel_->setTitle (tx ("nv.set_title"));
    setGeneral_ = add<Text> (page, skin_, Token::TextDim2, 11.0f, true);
    setGeneral_->setText (tx ("nv.set_general"));
    langPill_ = add<Pill> (page, skin_);
    setNote_ = add<Text> (page, skin_, Token::TextDim2, 11.0f);
    setPresets_ = add<Text> (page, skin_, Token::TextDim2, 11.0f, true);
    setPresets_->setText (tx ("nv.set_presets"));
    setFolder_ = add<Text> (page, skin_, Token::TextDim, 11.0f);
    openPill_ = add<Pill> (page, skin_);
    openPill_->setText (tx ("nv.pr_folder"));
    openPill_->onClick = [this] { auto dir = userPresetsDir(); dir.createDirectory(); dir.revealToUser(); };
    setAbout_ = add<Text> (page, skin_, Token::TextDim2, 11.0f, true);
    setAbout_->setText (tx ("nv.set_about"));
    setAboutText_ = add<Text> (page, skin_, Token::TextDim, 11.0f);
    setAboutText_->setText (tx ("nv.set_about_text"));
    setFolder_->setText (userPresetsDir().getFullPathName());

    const bool canApply = options_.brand && options_.getSettings && options_.applySettings;
    langPill_->setEnabled (canApply);
    setNote_->setText (canApply ? tx ("nv.set_note_header") : tx ("nv.set_note_insert"));
    langPill_->onClick = [this]
    {
        auto st = options_.getSettings();
        st.language = st.language == "fr" ? "en" : "fr";
        options_.applySettings (st);
    };
    refreshSettingsTexts();
}

void EditorBase::refreshSettingsTexts()
{
    const auto st = options_.getSettings ? options_.getSettings() : neptune::core::settings::GlobalSettings{};
    if (hdrTheme_ != nullptr)
    {
        hdrTheme_->setText (st.theme == "light" ? "LIGHT" : st.theme == "hc" ? "HIGH C." : st.theme == "hammond" ? "WOOD" : "DARK");
        hdrContrast_->setText (juce::String (st.contrast));
        hdrContrast_->setDim (st.theme == "hc");   // sans effet sur le thème à contraste élevé
        hdrMinus_->setDim (st.contrast <= 0);
        hdrPlus_->setDim (st.contrast >= 4);
    }
    if (settingsPage_ >= 0) langPill_->setText (tx ("nv.set_language") + "  " + juce::String (st.language).toUpperCase());
}

void EditorBase::layoutHeaderControls()
{
    if (reserveOverlay_ != nullptr) reserveOverlay_->setBounds (getLocalBounds());
    if (hdrAuto_ != nullptr)
    {
        const float wd = options_.stretchAuto ? 1160.0f : 1160.0f + extraW_;
        const float left = hdrTheme_ != nullptr ? wd - 14 - 36 - 10 - 64 - 10 - 28 - 36 - 28 - 12 : wd - 14 - 36 - 12;   // à gauche du sélecteur de contraste, ou du bypass
        hdrAuto_->setBounds (R (left - 96, 15, 96, 28));
    }
    if (hdrTheme_ == nullptr) return;
    // de droite à gauche : bypass (36 px à 14 px du bord), thème, puis le sélecteur de contraste « - valeur + »
    const float w = options_.stretchAuto ? 1160.0f : 1160.0f + extraW_;   // en stretchAuto, R() décale déjà l'en-tête
    hdrTheme_->setBounds (R (w - 14 - 36 - 10 - 64, 15, 64, 28));
    hdrPlus_->setBounds (R (w - 14 - 36 - 10 - 64 - 10 - 28, 15, 28, 28));
    hdrContrast_->setBounds (R (w - 14 - 36 - 10 - 64 - 10 - 28 - 36, 15, 36, 28));
    hdrMinus_->setBounds (R (w - 14 - 36 - 10 - 64 - 10 - 28 - 36 - 28, 15, 28, 28));
}

void EditorBase::layoutSettings()
{
    if (settingsPage_ < 0) return;
    setPanel_->setBounds (Rs (204, 72, 942, 638));
    setGeneral_->setBounds (R (220, 116, 400, 16));
    langPill_->setBounds (R (220, 140, 250, 34));
    setNote_->setBounds (R (220, 184, 900, 34));
    setPresets_->setBounds (R (220, 244, 400, 16));
    setFolder_->setBounds (R (220, 268, 900, 34));
    openPill_->setBounds (R (220, 310, 250, 34));
    setAbout_->setBounds (R (220, 384, 400, 16));
    setAboutText_->setBounds (R (220, 408, 900, 120));
}

} // namespace nova
