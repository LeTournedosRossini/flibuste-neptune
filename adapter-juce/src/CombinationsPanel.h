#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <functional>
#include <iterator>
#include <string>
#include "BarIcons.h"
#include "PluginProcessor.h"
#include "neptune/core/Combinations.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Theme.h"

// Écran des scènes, ex-combinaisons d'orgue (voir docs/specification-combinaisons.md et docs/plan-scenes.md) : en haut le modèle d'enregistrement (ce qu'un Set + piston mémorise) ; à gauche les 10 généraux de la banque courante (rappeler, renommer, effacer,
// les mettre dans la séquence) ou, dans l'onglet DIVISIONNELS, les 10 divisionnels du clavier choisi ; à droite la séquence des généraux (monter,
// descendre, retirer, vider) avec le pas courant marqué. Chaque changement est un
// geste annulable (la bibliothèque est dans le projet). On enregistre un général depuis la Façade (touche Set puis un piston) ou par un CC, pas ici.
class CombinationsPanel : public juce::Component, private juce::Timer
{
public:
    CombinationsPanel (NeptuneProcessor& processor, const neptune::core::theme::Palette& palette, const neptune::core::i18n::Translator& tr)
        : processor_ (processor), palette_ (palette), tr_ (tr), generalsModel_ (*this), sequenceModel_ (*this)
    {
        lib_ = processor_.combinations();
        status_ = processor_.combiStatus();
        title_.setText (text ("comb.title"), juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
        hint_.setText (text ("comb.hint"), juce::dontSendNotification);
        hint_.setFont (juce::FontOptions (13.0f));
        hint_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::TextDim));
        hint_.setJustificationType (juce::Justification::topLeft);
        bankLabel_.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        bankLabel_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Accent));
        bankLabel_.setJustificationType (juce::Justification::centred);
        generalsTitle_.setText (text ("comb.generals"), juce::dontSendNotification);
        sequenceTitle_.setText (text ("comb.sequence"), juce::dontSendNotification);
        for (auto* l : { &generalsTitle_, &sequenceTitle_ })
        {
            l->setFont (juce::FontOptions (14.0f, juce::Font::bold));
            l->setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
        }
        for (auto* l : std::initializer_list<juce::Label*> { &title_, &hint_, &bankLabel_, &generalsTitle_, &sequenceTitle_ }) addAndMakeVisible (*l);

        generalsTab_.setButtonText (text ("comb.tab_generals"));
        divisionalsTab_.setButtonText (text ("comb.tab_divisionals"));
        for (auto* b : { &generalsTab_, &divisionalsTab_ })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (7421);
            b->setColour (juce::TextButton::buttonOnColourId, colour (neptune::core::theme::Token::Accent));
        }
        generalsTab_.setToggleState (true, juce::dontSendNotification);
        generalsTab_.onClick = [this] { if (generalsTab_.getToggleState()) setDivisional (false); };
        divisionalsTab_.onClick = [this] { if (divisionalsTab_.getToggleState()) setDivisional (true); };
        keyboardLabel_.setText (text ("comb.keyboard"), juce::dontSendNotification);
        keyboardLabel_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
        for (int t = 0; t < neptune::core::mixer::numTracks; ++t)
        {
            if (! processor_.trackActive (t)) continue;
            const auto name = processor_.trackSettings (t).name;
            keyboardBox_.addItem (text ("comb.track") + " " + juce::String (t + 1) + (name.empty() ? juce::String() : " - " + utf8 (name)), t + 1);
        }
        keyboardBox_.setSelectedId (processor_.selectedTrack() + 1, juce::dontSendNotification);
        if (keyboardBox_.getSelectedId() == 0 && keyboardBox_.getNumItems() > 0) keyboardBox_.setSelectedItemIndex (0, juce::dontSendNotification);
        track_ = juce::jmax (0, keyboardBox_.getSelectedId() - 1);
        keyboardBox_.onChange = [this] { track_ = juce::jmax (0, keyboardBox_.getSelectedId() - 1); refresh (true); };
        for (auto* c : std::initializer_list<juce::Component*> { &generalsTab_, &divisionalsTab_, &keyboardLabel_, &keyboardBox_ }) addAndMakeVisible (*c);
        keyboardLabel_.setVisible (false);
        keyboardBox_.setVisible (false);

        prevBank_.setButtonText ("<");
        nextBank_.setButtonText (">");
        prevBank_.onClick = [this] { processor_.uiCombiSetBank (processor_.combiStatus().bank - 1); refresh (true); };
        nextBank_.onClick = [this] { processor_.uiCombiSetBank (processor_.combiStatus().bank + 1); refresh (true); };
        store_.setButtonText (text ("comb.store"));
        store_.onClick = [this] { storeSelected(); };
        recall_.setButtonText (text ("comb.recall"));
        recall_.onClick = [this] { recallSelected(); };
        rename_.setButtonText (text ("comb.rename"));
        rename_.onClick = [this] { renameSelected(); };
        erase_.setButtonText (text ("comb.erase"));
        erase_.onClick = [this] { eraseSelected(); };
        addToSequence_.setIcon (IconButton::Icon::Right);
        addToSequence_.setTooltip (text ("comb.add_seq_tip"));
        addToSequence_.onClick = [this] { addSelectedToSequence(); };
        up_.setButtonText (text ("comb.up"));
        up_.onClick = [this] { moveStep (-1); };
        down_.setButtonText (text ("comb.down"));
        down_.onClick = [this] { moveStep (1); };
        remove_.setIcon (IconButton::Icon::Cross);
        remove_.setTooltip (text ("comb.remove"));
        remove_.onClick = [this] { removeStep(); };
        clear_.setButtonText (text ("comb.clear_seq"));
        clear_.onClick = [this] { clearSequence(); };
        close_.setButtonText (text ("modal.close"));
        close_.onClick = [this] { if (onClose) onClose(); };
        for (auto* b : std::initializer_list<juce::Component*> { &prevBank_, &nextBank_, &store_, &recall_, &rename_, &erase_, &addToSequence_, &up_, &down_, &remove_, &clear_, &close_ }) addAndMakeVisible (*b);

        captureLabel_.setText (text ("comb.capture"), juce::dontSendNotification);
        captureLabel_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
        static const char* const templateKeys[] = { "comb.tpl_organ_console", "comb.tpl_organ_stops", "comb.tpl_mixer", "comb.tpl_everything", "comb.tpl_custom" };
        for (int i = 0; i < 5; ++i) captureBox_.addItem (text (templateKeys[i]), i + 1);
        captureBox_.onChange = [this]
        {
            auto lib = lib_;
            lib.capture.tpl = static_cast<neptune::core::combi::Template> (captureBox_.getSelectedId() - 1);
            if (lib.capture.tpl == neptune::core::combi::Template::Custom && lib.capture.families == 0 && lib.capture.scopes == 0)
            {
                // première fois : un point de départ utile (les jeux d'orgue et les tirants du routeur)
                lib.capture.families = (1u << static_cast<unsigned> (neptune::core::paramgroups::Family::Modulators)) | (1u << static_cast<unsigned> (neptune::core::paramgroups::Family::Router));
                lib.capture.scopes = (1u << static_cast<unsigned> (neptune::core::paramgroups::Scope::Track)) | (1u << static_cast<unsigned> (neptune::core::paramgroups::Scope::Global));
            }
            edit (std::move (lib), "Scene: record model");
        };
        glideLabel_.setText (text ("comb.glide"), juce::dontSendNotification);
        glideLabel_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Text));
        glideBox_.addItem (text ("comb.glide_0"), 1);
        for (std::size_t i = 1; i < std::size (glideTimes); ++i) glideBox_.addItem (juce::String (glideTimes[i]) + " ms", static_cast<int> (i) + 1);
        glideBox_.onChange = [this]
        {
            auto lib = lib_;
            lib.glideMs = glideTimes[static_cast<std::size_t> (juce::jlimit (1, static_cast<int> (std::size (glideTimes)), glideBox_.getSelectedId()) - 1)];
            edit (std::move (lib), "Scene: glide");
        };
        captureEdit_.setButtonText (text ("comb.custom_edit"));
        captureEdit_.onClick = [this] { showCustomMenu(); };
        insertStep_.setButtonText (text ("comb.insert_step"));
        insertStep_.onClick = [this] { insertStep(); };
        deleteStep_.setButtonText (text ("comb.delete_step"));
        deleteStep_.onClick = [this] { processor_.uiCombiSequenceDeleteCurrent(); refresh (true); };
        prevStepButton_.setButtonText ("<");
        nextStepButton_.setButtonText (">");
        prevStepButton_.setTooltip (text ("comb.prev_step"));
        nextStepButton_.setTooltip (text ("comb.next_step"));
        prevStepButton_.onClick = [this] { processor_.uiCombiCommand (neptune::core::combi::kCommandPrevious, track_); refresh (true); };
        nextStepButton_.onClick = [this] { processor_.uiCombiCommand (neptune::core::combi::kCommandNext, track_); refresh (true); };
        stepLabel_.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        stepLabel_.setColour (juce::Label::textColourId, colour (neptune::core::theme::Token::Accent));
        stepLabel_.setJustificationType (juce::Justification::centredRight);
        for (auto* c : std::initializer_list<juce::Component*> { &captureLabel_, &captureBox_, &captureEdit_, &glideLabel_, &glideBox_, &insertStep_, &deleteStep_, &prevStepButton_, &nextStepButton_, &stepLabel_ }) addAndMakeVisible (*c);

        generals_.setModel (&generalsModel_);
        generals_.setRowHeight (30);
        generals_.setColour (juce::ListBox::backgroundColourId, colour (neptune::core::theme::Token::BgApp));
        sequence_.setModel (&sequenceModel_);
        sequence_.setRowHeight (26);
        sequence_.setColour (juce::ListBox::backgroundColourId, colour (neptune::core::theme::Token::BgApp));
        addAndMakeVisible (generals_);
        addAndMakeVisible (sequence_);
        generals_.selectRow (0);
        setWantsKeyboardFocus (true);
        refresh (true);
        startTimerHz (4);
    }
    ~CombinationsPanel() override { stopTimer(); generals_.setModel (nullptr); sequence_.setModel (nullptr); }

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
        hint_.setBounds (r.removeFromTop (104));
        {
            auto row = r.removeFromTop (34);
            captureLabel_.setBounds (row.removeFromLeft (100));
            captureBox_.setBounds (row.removeFromLeft (470));
            row.removeFromLeft (8);
            captureEdit_.setBounds (row.removeFromLeft (120));
        }
        r.removeFromTop (6);
        {
            auto row = r.removeFromTop (34);
            glideLabel_.setBounds (row.removeFromLeft (100));
            glideBox_.setBounds (row.removeFromLeft (200));
        }
        r.removeFromTop (6);
        auto bank = r.removeFromTop (34);
        prevBank_.setBounds (bank.removeFromLeft (48));
        bankLabel_.setBounds (bank.removeFromLeft (200));
        nextBank_.setBounds (bank.removeFromLeft (48));
        bank.removeFromLeft (24);
        generalsTab_.setBounds (bank.removeFromLeft (140).reduced (2, 0));
        divisionalsTab_.setBounds (bank.removeFromLeft (150).reduced (2, 0));
        r.removeFromTop (6);
        {
            auto keyboardRow = r.removeFromTop (divisional_ ? 34 : 0);
            keyboardLabel_.setBounds (keyboardRow.removeFromLeft (90));
            keyboardBox_.setBounds (keyboardRow.removeFromLeft (320));
        }
        r.removeFromTop (divisional_ ? 6 : 2);
        close_.setBounds (r.removeFromBottom (36).removeFromRight (140));
        r.removeFromBottom (8);
        auto left = r.removeFromLeft ((r.getWidth() - 72) / 2);
        auto gutter = r.removeFromLeft (72);   // entre les deux listes : « → » ajoute la scène choisie au pas à pas, « ✕ » retire le pas choisi
        auto right = r;
        {
            const auto middle = gutter.getCentre();
            addToSequence_.setBounds (juce::Rectangle<int> (44, 34).withCentre ({ middle.x, middle.y - 24 }));
            remove_.setBounds (juce::Rectangle<int> (44, 34).withCentre ({ middle.x, middle.y + 24 }));
        }
        generalsTitle_.setBounds (left.removeFromTop (26));
        auto leftButtons = left.removeFromBottom (36);
        left.removeFromBottom (4);
        auto storeRow = left.removeFromBottom (36);
        left.removeFromBottom (6);
        generals_.setBounds (left);
        store_.setBounds (storeRow.reduced (2, 0));
        const int lw = leftButtons.getWidth() / 3;
        recall_.setBounds (leftButtons.removeFromLeft (lw).reduced (2, 0));
        rename_.setBounds (leftButtons.removeFromLeft (lw).reduced (2, 0));
        erase_.setBounds (leftButtons.reduced (2, 0));
        {
            auto titleRow = right.removeFromTop (26);
            stepLabel_.setBounds (titleRow.removeFromRight (120));
            nextStepButton_.setBounds (titleRow.removeFromRight (34).reduced (2, 0));
            prevStepButton_.setBounds (titleRow.removeFromRight (34).reduced (2, 0));
            sequenceTitle_.setBounds (titleRow);
        }
        auto stepButtons = right.removeFromBottom (36);
        right.removeFromBottom (4);
        auto rightButtons = right.removeFromBottom (36);
        right.removeFromBottom (6);
        sequence_.setBounds (right);
        insertStep_.setBounds (stepButtons.removeFromLeft (stepButtons.getWidth() / 2).reduced (2, 0));
        deleteStep_.setBounds (stepButtons.reduced (2, 0));
        const int rw = rightButtons.getWidth() / 3;
        up_.setBounds (rightButtons.removeFromLeft (rw).reduced (2, 0));
        down_.setBounds (rightButtons.removeFromLeft (rw).reduced (2, 0));
        clear_.setBounds (rightButtons.reduced (2, 0));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
        return false;
    }

private:
    juce::String text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }
    juce::Colour colour (neptune::core::theme::Token t) const
    {
        const auto& c = palette_.color (t);
        return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
    }
    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (940, juce::jmin (770, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    static juce::String utf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

    // La mémoire du piston `piston` de la banque courante : un général, ou un divisionnel du clavier choisi.
    const neptune::core::combi::Memory* memoryAt (int piston) const
    {
        return divisional_ ? lib_.divisional (track_, status_.bank, piston) : lib_.general (status_.bank, piston);
    }

    // « G3  Plein-jeu » ou « G3  (vide) » pour un piston de la banque courante (« D3 » pour un divisionnel).
    juce::String generalText (int piston) const
    {
        const auto* m = memoryAt (piston);
        juce::String t = juce::String (divisional_ ? "D" : "G") + juce::String (piston + 1) + "   ";
        if (m == nullptr) return t + text ("comb.empty");
        return t + (m->name.empty() ? juce::String ("-") : utf8 (m->name));
    }

    // « 2   B1 G3  Plein-jeu » pour un pas de la séquence ; le pas courant est précédé de « > ».
    juce::String stepText (int index) const
    {
        namespace cb = neptune::core::combi;
        const int key = lib_.sequence[static_cast<std::size_t> (index)];
        const int bank = key / cb::kPistons, piston = key % cb::kPistons;
        const auto* m = lib_.general (bank, piston);
        juce::String t = juce::String (index == status_.sequencePosition ? "> " : "   ") + juce::String (index + 1) + "   B" + juce::String (bank + 1) + " G"
                       + juce::String (piston + 1) + "   ";
        if (m == nullptr) return t + text ("comb.empty");
        return t + (m->name.empty() ? juce::String ("-") : utf8 (m->name));
    }

    int selectedKey() const   // la clef de la mémoire choisie (général ou divisionnel selon l'onglet)
    {
        const int row = generals_.getSelectedRow();
        if (row < 0) return -1;
        return divisional_ ? neptune::core::combi::divisionalKey (track_, status_.bank, row) : neptune::core::combi::generalKey (status_.bank, row);
    }

    void setDivisional (bool divisional)
    {
        divisional_ = divisional;
        keyboardLabel_.setVisible (divisional);
        keyboardBox_.setVisible (divisional);
        generalsTitle_.setText (text (divisional ? "comb.divisionals" : "comb.generals"), juce::dontSendNotification);
        resized();
        refresh (true);
    }

    void edit (neptune::core::combi::Library lib, const char* name)
    {
        processor_.uiEditCombinations (std::move (lib), name);
        refresh (true);
    }

    void recallSelected()
    {
        const int row = generals_.getSelectedRow();
        if (row < 0) return;
        processor_.uiCombiCommand ((divisional_ ? neptune::core::combi::kCommandDivisional : neptune::core::combi::kCommandGeneral) + row, track_);
        refresh (true);
    }

    void renameSelected()
    {
        namespace cb = neptune::core::combi;
        const int key = selectedKey();
        const auto* m = key < 0 ? nullptr : memoryAt (key % cb::kPistons);
        if (m == nullptr) return;
        const bool divisional = divisional_;
        auto* w = new juce::AlertWindow (text ("comb.rename"), text ("comb.rename_prompt"), juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", utf8 (m->name));
        w->addButton (text ("grp.ok"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (text ("mix.cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([w, key, divisional, safe = juce::Component::SafePointer<CombinationsPanel> (this)] (int result)
        {
            if (result != 1 || safe == nullptr) return;
            auto lib = safe->lib_;
            const auto typed = w->getTextEditorContents ("name").toStdString();
            if (divisional ? cb::renameDivisional (lib, key, typed) : cb::renameGeneral (lib, key, typed)) safe->edit (std::move (lib), "Scene: rename");
        }), true);
    }

    void eraseSelected()
    {
        const int key = selectedKey();
        if (key < 0) return;
        auto lib = lib_;
        if (divisional_ ? neptune::core::combi::eraseDivisional (lib, key) : neptune::core::combi::eraseGeneral (lib, key)) edit (std::move (lib), "Scene: erase");
    }

    void addSelectedToSequence()
    {
        const int key = selectedKey();
        if (key < 0) return;
        auto lib = lib_;
        if (! neptune::core::combi::sequenceAdd (lib, key)) return;
        const int last = static_cast<int> (lib.sequence.size()) - 1;
        edit (std::move (lib), "Scene: sequence");
        sequence_.selectRow (last);
    }

    // Mémorise l'état actuel de Neptune, selon le modèle d'enregistrement, dans la scène choisie (comme Set puis le piston, sans quitter l'écran) ; demande avant d'écraser.
    void storeSelected()
    {
        const int row = generals_.getSelectedRow();
        if (row < 0) return;
        const auto doStore = [safe = juce::Component::SafePointer<CombinationsPanel> (this), row]
        {
            if (safe == nullptr) return;
            namespace cb = neptune::core::combi;
            if (! safe->processor_.combiStatus().setArmed) safe->processor_.uiCombiCommand (cb::kCommandSet, safe->track_);   // arme Set (s'il ne l'est pas déjà)
            safe->processor_.uiCombiCommand ((safe->divisional_ ? cb::kCommandDivisional : cb::kCommandGeneral) + row, safe->track_);   // le piston enregistre
            safe->refresh (true);
        };
        if (memoryAt (row) == nullptr) { doStore(); return; }
        juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, text ("comb.title"), text ("comb.store_confirm"),
                                                                                    text ("grp.ok"), text ("mix.cancel"), this),
                                      [doStore] (int result) { if (result == 1) doStore(); });
    }

    void insertStep()
    {
        const int key = selectedKey();
        if (key < 0 || divisional_) return;
        processor_.uiCombiSequenceInsert (key);
        refresh (true);
        sequence_.selectRow (processor_.combiStatus().sequencePosition);
    }

    // Choix libre de ce qu'un enregistrement mémorise : des familles de paramètres et des portées (pistes, pistes parallèles, bus, Master, global). Le menu se rouvre après chaque case pour pouvoir en cocher plusieurs.
    void showCustomMenu()
    {
        namespace cb = neptune::core::combi;
        namespace pg = neptune::core::paramgroups;
        juce::PopupMenu menu;
        menu.addSectionHeader (text ("comb.custom_families"));
        for (int f = 0; f < pg::numFamilies - 1; ++f)   // sans « Autre »
            menu.addItem (100 + f, text (pg::familyKey (static_cast<pg::Family> (f))), true, (lib_.capture.families & (1u << static_cast<unsigned> (f))) != 0);
        menu.addSeparator();
        menu.addSectionHeader (text ("comb.custom_scopes"));
        static const char* const scopeKeys[] = { "pfilter.track", "pfilter.parallels", "pfilter.buses", "pfilter.master", "pfilter.global" };
        for (int s = 0; s < 5; ++s) menu.addItem (200 + s, text (scopeKeys[s]), true, (lib_.capture.scopes & (1u << static_cast<unsigned> (s))) != 0);
        menu.setLookAndFeel (&getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&captureEdit_), [safe = juce::Component::SafePointer<CombinationsPanel> (this)] (int chosen)
        {
            if (safe == nullptr || chosen <= 0) return;
            auto lib = safe->lib_;
            lib.capture.tpl = cb::Template::Custom;
            if (chosen >= 100 && chosen < 200) lib.capture.families ^= 1u << static_cast<unsigned> (chosen - 100);
            else if (chosen >= 200) lib.capture.scopes ^= 1u << static_cast<unsigned> (chosen - 200);
            safe->edit (std::move (lib), "Scene: record model");
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->showCustomMenu(); });
        });
    }

    void moveStep (int delta)
    {
        const int row = sequence_.getSelectedRow();
        auto lib = lib_;
        if (! neptune::core::combi::sequenceMove (lib, row, delta)) return;
        edit (std::move (lib), "Scene: sequence");
        sequence_.selectRow (row + delta);
    }

    void removeStep()
    {
        const int row = sequence_.getSelectedRow();
        auto lib = lib_;
        if (! neptune::core::combi::sequenceRemove (lib, row)) return;
        edit (std::move (lib), "Scene: sequence");
        sequence_.selectRow (juce::jmin (row, static_cast<int> (lib_.sequence.size()) - 1));
    }

    void clearSequence()
    {
        if (lib_.sequence.empty()) return;
        auto lib = lib_;
        lib.sequence.clear();
        edit (std::move (lib), "Scene: sequence");
    }

    void updateButtons()
    {
        namespace cb = neptune::core::combi;
        const int row = generals_.getSelectedRow();
        const bool filled = row >= 0 && memoryAt (row) != nullptr;
        store_.setEnabled (row >= 0);
        recall_.setEnabled (filled);
        rename_.setEnabled (filled);
        erase_.setEnabled (filled);
        addToSequence_.setEnabled (! divisional_ && row >= 0 && lib_.sequence.size() < static_cast<std::size_t> (cb::kMaxSequence));   // la séquence ne contient que des généraux
        const int step = sequence_.getSelectedRow();
        const int n = static_cast<int> (lib_.sequence.size());
        up_.setEnabled (step > 0 && step < n);
        down_.setEnabled (step >= 0 && step < n - 1);
        remove_.setEnabled (step >= 0 && step < n);
        clear_.setEnabled (n > 0);
        insertStep_.setEnabled (! divisional_ && filled && n < cb::kMaxSequence);
        deleteStep_.setEnabled (status_.sequencePosition >= 0 && status_.sequencePosition < n);
        prevBank_.setEnabled (status_.bank > 0);
        nextBank_.setEnabled (status_.bank < cb::kBanks - 1);
    }

    // Relit l'état du processeur ; ne redessine que si quelque chose a changé (ou si `force`).
    void refresh (bool force)
    {
        auto lib = processor_.combinations();
        const auto status = processor_.combiStatus();
        const bool changed = force || lib != lib_ || status.bank != status_.bank || status.sequencePosition != status_.sequencePosition;
        lib_ = std::move (lib);
        status_ = status;
        if (! changed) return;
        bankLabel_.setText (juce::String::fromUTF8 (tr_.format ("comb.bank", { std::to_string (status_.bank + 1), std::to_string (neptune::core::combi::kBanks) }).c_str()),
                            juce::dontSendNotification);
        captureBox_.setSelectedId (static_cast<int> (lib_.capture.tpl) + 1, juce::dontSendNotification);
        captureEdit_.setVisible (lib_.capture.tpl == neptune::core::combi::Template::Custom);
        {
            int item = 1;
            for (std::size_t i = 0; i < std::size (glideTimes); ++i) if (glideTimes[i] == lib_.glideMs) item = static_cast<int> (i) + 1;
            glideBox_.setSelectedId (item, juce::dontSendNotification);
        }
        stepLabel_.setText (juce::String::fromUTF8 (tr_.format ("comb.step", { std::to_string (status_.sequencePosition + 1), std::to_string (lib_.sequence.size()) }).c_str()), juce::dontSendNotification);
        generals_.updateContent();
        sequence_.updateContent();
        generals_.repaint();
        sequence_.repaint();
        updateButtons();
    }

    void timerCallback() override { refresh (false); }

    struct GeneralsModel : juce::ListBoxModel
    {
        explicit GeneralsModel (CombinationsPanel& o) : owner (o) {}
        int getNumRows() override { return neptune::core::combi::kPistons; }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            using neptune::core::theme::Token;
            if (selected) { g.setColour (owner.colour (Token::Accent).withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
            const bool filled = owner.memoryAt (row) != nullptr;
            g.setColour (filled ? owner.colour (Token::Text) : owner.colour (Token::TextDim));
            g.setFont (juce::FontOptions (16.0f));
            g.drawText (owner.generalText (row), 10, 0, w - 20, h, juce::Justification::centredLeft);
        }
        void selectedRowsChanged (int) override { owner.updateButtons(); }
        void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { owner.recallSelected(); }
        CombinationsPanel& owner;
    };

    struct SequenceModel : juce::ListBoxModel
    {
        explicit SequenceModel (CombinationsPanel& o) : owner (o) {}
        int getNumRows() override { return static_cast<int> (owner.lib_.sequence.size()); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            using neptune::core::theme::Token;
            if (row < 0 || row >= static_cast<int> (owner.lib_.sequence.size())) return;
            if (selected) { g.setColour (owner.colour (Token::Accent).withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
            g.setColour (row == owner.status_.sequencePosition ? owner.colour (Token::Accent) : owner.colour (Token::Text));
            g.setFont (juce::FontOptions (15.0f));
            g.drawText (owner.stepText (row), 10, 0, w - 20, h, juce::Justification::centredLeft);
        }
        void selectedRowsChanged (int) override { owner.updateButtons(); }
        CombinationsPanel& owner;
    };

    NeptuneProcessor& processor_;
    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    neptune::core::combi::Library lib_;
    NeptuneProcessor::CombiStatus status_;
    GeneralsModel generalsModel_;
    SequenceModel sequenceModel_;
    bool divisional_ = false;      // onglet DIVISIONNELS : les mémoires montrées sont celles du clavier `track_`
    int track_ = 0;
    juce::TextButton generalsTab_, divisionalsTab_;
    static constexpr int glideTimes[] = { 0, 50, 100, 200, 300, 500, 1000, 2000 };
    juce::Label keyboardLabel_, captureLabel_, glideLabel_, stepLabel_;
    juce::ComboBox keyboardBox_, captureBox_, glideBox_;
    juce::TextButton captureEdit_, insertStep_, deleteStep_, prevStepButton_, nextStepButton_, store_;
    juce::Label title_, hint_, bankLabel_, generalsTitle_, sequenceTitle_;
    juce::TextButton prevBank_, nextBank_, recall_, rename_, erase_, up_, down_, clear_, close_;
    IconButton addToSequence_, remove_;
    juce::ListBox generals_, sequence_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CombinationsPanel)
};
