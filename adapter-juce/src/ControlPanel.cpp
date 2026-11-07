#include "ControlPanel.h"

#include <algorithm>

#include "CombiText.h"
#include "neptune/core/Osc.h"

using namespace neptune::core;
using neptune::core::theme::Token;
namespace ctl = neptune::core::control;

namespace
{
constexpr int kRowH = 30;
constexpr int kActionBase = 100, kTargetBase = 1000;
constexpr int kSourceLearn = 1, kSourceOsc = 2, kSourceMacroBase = 100;

const char* actionKey (ctl::Action a)
{
    switch (a)
    {
        case ctl::Action::PresetNext: return "cpanel.action_preset_next";
        case ctl::Action::PresetPrevious: return "cpanel.action_preset_previous";
        case ctl::Action::Panic: return "cpanel.action_panic";
        case ctl::Action::TrackNext: return "cpanel.action_track_next";
        case ctl::Action::TrackPrevious: return "cpanel.action_track_previous";
        default: return "cpanel.choose_target";
    }
}

// Arbre de sous-menus construit sur les segments des adresses OSC (« track / 3 / organ / reverb »).
struct Node
{
    std::string name;
    int id = 0;                   // feuille : identifiant de menu
    std::vector<Node> children;   // ordre d'insertion (celui du catalogue : pistes dans l'ordre, pas alphabétique)
};

void insertPath (Node& root, const std::string& path, int id)
{
    Node* n = &root;
    std::size_t pos = 0;
    while (pos <= path.size())
    {
        const auto end = path.find ('/', pos);
        const auto seg = path.substr (pos, end == std::string::npos ? std::string::npos : end - pos);
        auto it = std::find_if (n->children.begin(), n->children.end(), [&] (const Node& c) { return c.name == seg; });
        if (it == n->children.end())
        {
            n->children.push_back ({ seg, 0, {} });
            it = n->children.end() - 1;
        }
        n = &*it;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    n->id = id;
}

juce::PopupMenu buildMenu (const Node& node, ParamId current, const std::vector<ParamId>& ids)
{
    juce::PopupMenu menu;
    for (const auto& c : node.children)
    {
        if (c.children.empty())
        {
            const bool ticked = c.id >= kTargetBase && ids[static_cast<std::size_t> (c.id - kTargetBase)] == current;
            menu.addItem (c.id, juce::String::fromUTF8 (c.name.c_str()), true, ticked);
        }
        else menu.addSubMenu (juce::String::fromUTF8 (c.name.c_str()), buildMenu (c, current, ids));
    }
    return menu;
}
} // namespace

struct ControlPanel::Row
{
    juce::TextButton source, target, remove;
    juce::Slider lo, hi, curve;
    juce::ToggleButton invert;
    juce::ComboBox mode;
};

class ControlPanel::Content : public juce::Component
{
public:
    juce::OwnedArray<Row> rows;
    void resized() override
    {
        int y = 0;
        for (auto* r : rows)
        {
            auto line = juce::Rectangle<int> (0, y, getWidth(), kRowH);
            r->source.setBounds (line.removeFromLeft (130).reduced (0, 2));
            line.removeFromLeft (4);
            r->target.setBounds (line.removeFromLeft (250).reduced (0, 2));
            line.removeFromLeft (4);
            r->lo.setBounds (line.removeFromLeft (70).reduced (0, 3));
            line.removeFromLeft (4);
            r->hi.setBounds (line.removeFromLeft (70).reduced (0, 3));
            line.removeFromLeft (4);
            r->curve.setBounds (line.removeFromLeft (64).reduced (0, 3));
            line.removeFromLeft (8);
            r->invert.setBounds (line.removeFromLeft (30).reduced (0, 3));
            line.removeFromLeft (4);
            r->mode.setBounds (line.removeFromLeft (130).reduced (0, 3));
            line.removeFromLeft (6);
            r->remove.setBounds (line.removeFromLeft (28).reduced (0, 2));
            y += kRowH;
        }
    }
};

juce::Colour ControlPanel::colour (Token t) const
{
    const auto& c = palette_.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

juce::String ControlPanel::text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }

juce::String ControlPanel::actionText (ctl::Action a) const
{
    const int command = ctl::combinationCommand (a);
    if (command <= 0) return text (actionKey (a));
    return combiCommandText ([this] (const char* key) { return text (key); }, command);
}

ControlPanel::ControlPanel (NeptuneProcessor& p, const theme::Palette& palette, const i18n::Translator& tr)
    : processor_ (p), palette_ (palette), tr_ (tr), content_ (std::make_unique<Content>())
{
    newButton_.setButtonText (text ("cpanel.new"));
    newButton_.onClick = [this] { addAssignment(); };
    addAndMakeVisible (newButton_);
    emptyLabel_.setText (text ("cpanel.empty"), juce::dontSendNotification);
    emptyLabel_.setColour (juce::Label::textColourId, colour (Token::TextDim));
    emptyLabel_.setJustificationType (juce::Justification::centred);
    addChildComponent (emptyLabel_);
    viewport_.setViewedComponent (content_.get(), false);
    viewport_.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport_);
    setSize (panelWidth, panelHeight);
    config_ = processor_.controlConfig();
    rebuild();
    startTimerHz (10);
}

ControlPanel::~ControlPanel()
{
    stopTimer();
    processor_.control().setLearning (false);
}

const ctl::Binding* ControlPanel::at (std::size_t i) const
{
    if (i < config_.bindings.size()) return &config_.bindings[i];
    return hasPending_ && i == config_.bindings.size() ? &pending_ : nullptr;
}

juce::String ControlPanel::targetLabel (const ctl::Binding& b) const
{
    if (b.target.isAction) return text ("cpanel.action") + ": " + actionText (b.target.action);
    const auto* spec = processor_.parameters().find (b.target.param);
    if (spec == nullptr) return text ("cpanel.choose_target");
    const auto& address = osc::addressFor (b.target.param);
    // le nom de l'hôte est ambigu entre les familles : l'adresse hiérarchique le précise
    return juce::String::fromUTF8 ((address.empty() ? spec->name : address.substr (std::string (osc::kPrefix).size() + 1)).c_str());
}

void ControlPanel::rebuild()
{
    updating_ = true;
    content_->rows.clear();
    for (std::size_t i = 0; i < count(); ++i)
    {
        auto* row = content_->rows.add (new Row);
        const auto& b = *at (i);
        row->source.setButtonText (juce::String::fromUTF8 (ctl::sourceLabel (b.source).c_str()));
        row->source.setClickingTogglesState (false);
        row->source.onClick = [this, i, row] { showSourceMenu (i, row->source); };
        row->target.setButtonText (targetLabel (b));
        row->target.onClick = [this, i, row] { showTargetMenu (i, row->target); };
        row->remove.setButtonText ("x");
        row->remove.onClick = [this, i] { remove (i); };
        auto setup = [this] (juce::Slider& s, double lo, double hi, double step, const juce::String& suffix)
        {
            s.setSliderStyle (juce::Slider::LinearBar);
            s.setRange (lo, hi, step);
            s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            s.setTextValueSuffix (suffix);
            s.setColour (juce::Slider::trackColourId, colour (Token::Accent).withAlpha (0.5f));
            s.setColour (juce::Slider::backgroundColourId, colour (Token::Grid));
            s.setColour (juce::Slider::textBoxTextColourId, colour (Token::Text));
        };
        setup (row->lo, 0.0, 100.0, 1.0, " %");
        setup (row->hi, 0.0, 100.0, 1.0, " %");
        setup (row->curve, 0.1, 10.0, 0.05, "");
        row->curve.setSkewFactorFromMidPoint (1.0);
        row->lo.setTooltip (text ("cpanel.min"));
        row->hi.setTooltip (text ("cpanel.max"));
        row->curve.setTooltip (text ("cpanel.curve"));
        row->lo.setValue (b.lo * 100.0, juce::dontSendNotification);
        row->hi.setValue (b.hi * 100.0, juce::dontSendNotification);
        row->curve.setValue (b.curve, juce::dontSendNotification);
        row->lo.setDoubleClickReturnValue (true, 0.0);
        row->hi.setDoubleClickReturnValue (true, 100.0);
        row->curve.setDoubleClickReturnValue (true, 1.0);
        row->lo.onValueChange = [this, i, row] { if (! updating_) edit (i, [row] (ctl::Binding& x) { x.lo = row->lo.getValue() / 100.0; }); };
        row->hi.onValueChange = [this, i, row] { if (! updating_) edit (i, [row] (ctl::Binding& x) { x.hi = row->hi.getValue() / 100.0; }); };
        row->curve.onValueChange = [this, i, row] { if (! updating_) edit (i, [row] (ctl::Binding& x) { x.curve = row->curve.getValue(); }); };
        row->invert.setButtonText ("");
        row->invert.setTooltip (text ("cpanel.invert"));
        row->invert.setToggleState (b.invert, juce::dontSendNotification);
        row->invert.onClick = [this, i, row] { edit (i, [row] (ctl::Binding& x) { x.invert = row->invert.getToggleState(); }); };
        row->mode.addItem (text ("cpanel.absolute"), 1);
        row->mode.addItem (text ("cpanel.pickup"), 2);
        row->mode.addItem (text ("cpanel.relative_twos"), 3);
        row->mode.addItem (text ("cpanel.relative_offset"), 4);
        row->mode.addItem (text ("cpanel.relative_sign"), 5);
        const bool relativeOk = b.source.kind == ctl::SourceKind::CC && ! b.target.isAction;   // les encodeurs relatifs : CC vers un paramètre
        for (int id = 3; id <= 5; ++id) row->mode.setItemEnabled (id, relativeOk);
        row->mode.setSelectedId (1 + static_cast<int> (b.mode), juce::dontSendNotification);   // Absolute, Pickup, RelativeTwos, RelativeOffset, RelativeSign
        row->mode.onChange = [this, i, row]
        {
            if (updating_) return;
            const auto id = row->mode.getSelectedId();
            edit (i, [id] (ctl::Binding& x) { x.mode = static_cast<ctl::Mode> (id - 1); });
        };
        for (juce::Component* c : std::initializer_list<juce::Component*> { &row->source, &row->target, &row->lo, &row->hi, &row->curve, &row->invert, &row->mode, &row->remove })
            content_->addAndMakeVisible (c);
    }
    if (learning_ >= 0 && learning_ < content_->rows.size()) content_->rows[learning_]->source.setButtonText (text ("cpanel.listening"));
    content_->setSize (viewport_.getMaximumVisibleWidth(), static_cast<int> (count()) * kRowH);
    content_->resized();
    emptyLabel_.setVisible (count() == 0);
    updating_ = false;
}

void ControlPanel::edit (std::size_t index, const std::function<void (ctl::Binding&)>& change)
{
    if (index < config_.bindings.size())
    {
        change (config_.bindings[index]);
        processor_.uiSetControlConfig (config_);   // la copie affichée est déjà à jour : pas de reconstruction
        return;
    }
    if (! hasPending_ || index != config_.bindings.size()) return;
    change (pending_);
    const bool complete = pending_.target.isAction ? pending_.target.action != ctl::Action::None : pending_.target.param != 0;
    if (! complete) return;
    // la cible est choisie : l'assignation devient réelle (le moteur ignore celles qui n'ont pas de cible)
    config_.bindings.push_back (pending_);
    hasPending_ = false;
    processor_.uiSetControlConfig (config_);
    rebuild();
}

void ControlPanel::addAssignment()
{
    if (hasPending_ || static_cast<int> (config_.bindings.size()) >= ctl::kMaxBindings) return;
    pending_ = ctl::Binding {};                // source CC apprise tout de suite ; cible à choisir (non enregistrée tant qu'elle manque)
    hasPending_ = true;
    rebuild();
    startLearning (static_cast<int> (count()) - 1);
    viewport_.setViewPosition (0, content_->getHeight());
}

void ControlPanel::remove (std::size_t index)
{
    if (index >= count()) return;
    if (learning_ >= 0) startLearning (-1);
    if (index >= config_.bindings.size()) hasPending_ = false;
    else
    {
        config_.bindings.erase (config_.bindings.begin() + static_cast<std::ptrdiff_t> (index));
        processor_.uiSetControlConfig (config_);
    }
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ControlPanel> (this)] { if (safe != nullptr) safe->rebuild(); });
}

void ControlPanel::startLearning (int index)
{
    learning_ = index;
    int c = 0, n = 0;
    processor_.control().takeLearnedCc (c, n);   // vide un éventuel ancien CC ou NRPN
    processor_.control().takeLearnedNrpn (c, n);
    processor_.control().setLearning (index >= 0);
    rebuild();
}

void ControlPanel::showSourceMenu (std::size_t index, juce::Component& anchor)
{
    juce::PopupMenu menu;
    menu.addItem (kSourceLearn, text ("cpanel.learn_cc"));
    juce::PopupMenu macros;
    for (int g0 = 0; g0 < ctl::kMacros; g0 += 16)
    {
        juce::PopupMenu group;
        for (int m = g0; m < g0 + 16; ++m) group.addItem (kSourceMacroBase + m, "Macro " + juce::String (m + 1));
        macros.addSubMenu ("Macros " + juce::String (g0 + 1) + " - " + juce::String (g0 + 16), group);
    }
    menu.addSubMenu (text ("cpanel.macro"), macros);
    menu.addItem (kSourceOsc, text ("cpanel.osc_address"));
    juce::Component::SafePointer<ControlPanel> safe (this);
    menu.setLookAndFeel (&anchor.getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [safe, index] (int choice)
    {
        if (safe == nullptr || choice == 0) return;
        if (choice == kSourceLearn) { safe->startLearning (static_cast<int> (index)); return; }
        if (choice >= kSourceMacroBase)
        {
            safe->edit (index, [choice] (ctl::Binding& b) { b.source = {}; b.source.kind = ctl::SourceKind::Macro; b.source.number = choice - kSourceMacroBase; });
            safe->rebuild();
            return;
        }
        // adresse OSC saisie
        auto* w = new juce::AlertWindow (safe->text ("cpanel.osc_address"), safe->text ("cpanel.osc_prompt"), juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("address", "/", "");
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, index, w] (int result)
        {
            const auto address = w->getTextEditorContents ("address").trim().toStdString();
            if (safe != nullptr && result == 1 && address.size() > 1 && address[0] == '/')
            {
                safe->edit (index, [&address] (ctl::Binding& b) { b.source = {}; b.source.kind = ctl::SourceKind::Osc; b.source.address = address; });
                safe->rebuild();
            }
        }), true);
    });
}

void ControlPanel::showTargetMenu (std::size_t index, juce::Component& anchor)
{
    if (index >= count()) return;
    const auto current = at (index)->target;
    const auto& set = processor_.parameters();
    // les paramètres de ce plugin qui ont une adresse, dans l'ordre du catalogue
    auto ids = std::make_shared<std::vector<ParamId>>();
    Node root;
    for (const auto id : osc::catalogIds())
    {
        if (set.indexOf (id) == ParameterSet::npos) continue;
        const auto& address = osc::addressFor (id);
        insertPath (root, address.substr (std::string (osc::kPrefix).size() + 1), kTargetBase + static_cast<int> (ids->size()));
        ids->push_back (id);
    }
    juce::PopupMenu menu = buildMenu (root, current.isAction ? 0 : current.param, *ids);
    menu.addSeparator();
    juce::PopupMenu actions, combinations;
    for (int a = 1; a < ctl::kNumActions; ++a)
        (a < ctl::kNumBaseActions ? actions : combinations).addItem (kActionBase + a, actionText (static_cast<ctl::Action> (a)), true, current.isAction && static_cast<int> (current.action) == a);
    actions.addSubMenu (text ("combi.menu"), combinations);
    menu.addSubMenu (text ("cpanel.actions"), actions);
    juce::Component::SafePointer<ControlPanel> safe (this);
    menu.setLookAndFeel (&anchor.getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor).withMaximumNumColumns (1), [safe, index, ids] (int choice)
    {
        if (safe == nullptr || choice == 0) return;
        if (choice >= kTargetBase)
        {
            const auto i = static_cast<std::size_t> (choice - kTargetBase);
            if (i >= ids->size()) return;
            const auto id = (*ids)[i];
            safe->edit (index, [id] (ctl::Binding& b) { b.target = {}; b.target.param = id; });
        }
        else safe->edit (index, [choice] (ctl::Binding& b) { b.target = {}; b.target.isAction = true; b.target.action = static_cast<ctl::Action> (choice - kActionBase); });
        safe->rebuild();
    });
}

void ControlPanel::timerCallback()
{
    // apprentissage : le dernier CC reçu devient la source de la ligne en cours
    if (learning_ >= 0)
    {
        int channel = 0, number = 0;
        int nrpnChannel = 0, nrpnNumber = 0;
        if (processor_.control().takeLearnedNrpn (nrpnChannel, nrpnNumber))
        {
            const int row = learning_;
            learning_ = -1;
            processor_.control().setLearning (false);
            edit (static_cast<std::size_t> (row), [nrpnNumber] (ctl::Binding& b) { b.source = {}; b.source.kind = ctl::SourceKind::Nrpn; b.source.number = nrpnNumber; });
            rebuild();
        }
        else if (processor_.control().takeLearnedCc (channel, number))
        {
            const int row = learning_;
            learning_ = -1;
            processor_.control().setLearning (false);
            edit (static_cast<std::size_t> (row), [number] (ctl::Binding& b) { b.source = {}; b.source.kind = ctl::SourceKind::CC; b.source.channel = 0; b.source.number = number; });
            (void) channel;   // toutes les voies : plus simple pour une surface de contrôle ; la voie se règle dans le projet (JSON)
            rebuild();
        }
    }
    // changement venu d'ailleurs (menu CC, annuler, chargement d'un état)
    const auto now = processor_.controlConfig();
    if (! (now == config_))   // (l'assignation en attente de cible n'est pas dans la configuration)
    {
        config_ = now;
        rebuild();
    }
}

void ControlPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto head = r.removeFromTop (28);
    newButton_.setBounds (head.removeFromRight (180));
    r.removeFromTop (18);   // en-têtes de colonnes (peintes)
    viewport_.setBounds (r);
    emptyLabel_.setBounds (r);
    content_->setSize (viewport_.getMaximumVisibleWidth(), static_cast<int> (count()) * kRowH);
}

void ControlPanel::paint (juce::Graphics& g)
{
    g.fillAll (colour (Token::BgCard));
    g.setColour (colour (Token::Accent));
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText (text ("cpanel.title"), 12, 10, getWidth() - 220, 22, juce::Justification::centredLeft);
    g.setColour (colour (Token::TextDim));
    g.setFont (juce::FontOptions (10.0f));
    const int y = 40;
    g.drawText (text ("cpanel.source"), 12, y, 130, 14, juce::Justification::centredLeft);
    g.drawText (text ("cpanel.target"), 146, y, 250, 14, juce::Justification::centredLeft);
    g.drawText (text ("cpanel.min"), 400, y, 70, 14, juce::Justification::centredLeft);
    g.drawText (text ("cpanel.max"), 474, y, 70, 14, juce::Justification::centredLeft);
    g.drawText (text ("cpanel.curve"), 548, y, 64, 14, juce::Justification::centredLeft);
    g.drawText (text ("cpanel.invert"), 620, y, 36, 14, juce::Justification::centredLeft);
    g.drawText (text ("cpanel.mode"), 660, y, 96, 14, juce::Justification::centredLeft);
}
