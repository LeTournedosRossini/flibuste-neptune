#pragma once
// Interface d'Orgamon (orgue à roues phoniques, moteur de setBfree) au style NOVA (maquette design/nova/ui/nova_hammond_console_ui.html), sur le socle commun
// nova::EditorBase : pages DRAWBARS (trois claviers de tirettes, percussion, vibrato / chorus, claviers MIDI et splits, clavier à l'écran, potards), LESLIE (vitesse,
// mélange, rotors), LIVE (tirettes agrandies, percussion, Leslie), MATRICE (grille de presets de la banque courante pour le concert), SETTINGS (accord, clic des touches,
// câblage du générateur, contrôleurs MIDI). La bibliothèque de presets (banques, programmes MIDI, gestionnaire, corbeille) est conservée : la pastille de preset de
// l'en-tête ouvre un menu NOVA et le gestionnaire existant se superpose à la fenêtre.
#include <functional>
#include <memory>
#include "NovaEditor.h"
#include "NovaPresetFields.h"
#include "NovaPresetUi.h"
#include "NovaWidgets.h"
#include "SynthKeyboard.h"
#include "neptune/core/OrganEngine.h"
#include "neptune/core/OrganPreset.h"

// Ce que l'éditeur demande à l'orgue : le même éditeur sert au plugin Orgamon et à une piste Orgue de Neptune.
struct OrganLink
{
    std::function<neptune::core::organ::OrganDescription()> description;
    std::function<void (neptune::core::organ::OrganDescription)> edit;   // annulable
    std::function<std::uint32_t()> undoRevision;
    std::function<void()> panic;
    std::function<void (int channel, int key)> noteOn, noteOff;
    std::function<std::uint64_t (int word)> heldWord;                       // touches tenues (bits 0..63 puis 64..127)
    std::function<int()> heldCount;
    std::function<float (int channel)> outPeak;                             // crête de sortie (0 gauche, 1 droite), amplitude linéaire
    std::function<float (int channel)> midiLevel;                           // vélocité du dernier note-on du canal MIDI (0..15), 0 si aucune note tenue
    std::function<bool()> ccEnabled;
    std::function<juce::String()> carteText;                                // « Carte : ON · canal 1 · Dm7 » (facultatif : sans lui, pas de bouton Carte)
    std::function<void (juce::Component*)> carteMenu;
    std::function<void (bool)> setCc;
    std::function<void()> collect;                                          // libère l'orgue remplacé (facultatif)
    int track = 0;
    neptune::core::ParamId volumeId = 0, gainId = 0;                        // potards de volume et de gain de la console
};

class NovaOrganEditor : public nova::EditorBase
{
public:
    NovaOrganEditor (ParamHost&, PresetHost&, OrganLink, const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle, const neptune::core::i18n::Translator&, Options);
    ~NovaOrganEditor() override;
    bool keyPressed (const juce::KeyPress&) override;
    void showPresetMenu (juce::Component* anchor);   // pastille de preset de l'en-tête
    void setPageForTest (int p) { setPage (p); }

private:
    class Drawbar;
    class MidiBar;
    class DescSlider;
    class NumPill;
    class TaperGrid;
    struct MenuPill;
    struct DrawbarSet;
    struct ToggleSet;
    struct LeslieSet;

    void layout() override;
    void tick() override;
    void paintBackground (juce::Graphics&) override;
    void buildLeftColumn();
    void buildDrawbarsPage();
    void buildLeslieAndLivePages();
    void buildMatrixPage();
    void buildSettingsExtras();
    DrawbarSet* makeDrawbars (int page);
    ToggleSet* makeToggles (int page, bool large);
    LeslieSet* makeLeslie (int page);
    MenuPill* makeMenuPill (int page, std::function<juce::String()> label, std::vector<juce::String> items, std::function<int()> get, std::function<void (int)> set);
    void editDesc (const std::function<void (neptune::core::organ::OrganDescription&)>&);

    OrganLink link_;
    PresetHost& presets_;
    int track_ = 0;
    std::unique_ptr<nova::PresetUi> ui_;
    neptune::core::organ::OrganDescription desc_;
    std::vector<std::unique_ptr<DrawbarSet>> owned_sets_;
    std::vector<std::unique_ptr<ToggleSet>> owned_toggles_;
    std::vector<std::unique_ptr<LeslieSet>> owned_leslie_;
    std::vector<std::unique_ptr<MenuPill>> owned_menus_;
    std::uint32_t lastUndoRevision_ = 0;

    // colonne de gauche
    nova::Panel *metersPanel_ = nullptr, *navPanel_ = nullptr, *lesliePanel_ = nullptr;
    std::vector<MidiBar*> outBars_;    // niveau de sortie gauche et droite (même taille que les barres MIDI)
    std::vector<MidiBar*> midiBars_;   // vu-mètres de contrôle des canaux MIDI des trois claviers (vélocité d'entrée)
    nova::Text* notesText_ = nullptr;
    LeslieSet* leftLeslie_ = nullptr;

    // page DRAWBARS
    nova::Panel *drawbarsPanel_ = nullptr, *percPanel_ = nullptr, *vibPanel_ = nullptr, *manualsPanel_ = nullptr, *keyboardPanel_ = nullptr;
    DrawbarSet* drawbars_ = nullptr;
    ToggleSet* toggles_ = nullptr;
    std::vector<MenuPill*> menuPills_;
    std::vector<NumPill*> numPills_;
    SynthKeyboard* keyboard_ = nullptr;
    nova::Pill *octDown_ = nullptr, *octUp_ = nullptr, *holdPill_ = nullptr;
    struct Column { nova::Panel* card; nova::KnobCard* knob; };
    std::vector<Column> columns_;
    nova::Pill* drivePill_ = nullptr;
    std::vector<MenuPill*> manualMenus_;   // canaux et transpositions (page DRAWBARS)

    // page LESLIE
    nova::Panel* lesliePagePanel_ = nullptr;
    LeslieSet* leslieSet_ = nullptr;
    nova::KnobCard* leslieMixCard_ = nullptr;
    std::vector<DescSlider*> leslieSliders_;

    // page LIVE
    nova::Panel *livePanel_ = nullptr, *liveLowerPanel_ = nullptr;
    DrawbarSet* liveDrawbars_ = nullptr;
    ToggleSet* liveToggles_ = nullptr;
    LeslieSet* liveLeslie_ = nullptr;

    // page MATRICE
    nova::Panel* matrixPanel_ = nullptr;
    nova::PresetTiles* tiles_ = nullptr;
    nova::PresetFields* fields_ = nullptr;
    nova::Text* fieldsTitle_ = nullptr;
    nova::Pill *colsPill_ = nullptr, *rowsPill_ = nullptr, *bankPrev_ = nullptr, *bankNext_ = nullptr, *bankPill_ = nullptr, *fieldPill_ = nullptr;

    // page SETTINGS
    nova::Panel* organ_settings_panel_ = nullptr;
    nova::Text *tuneSection_ = nullptr, *clickSection_ = nullptr, *wiringSection_ = nullptr, *midiText_ = nullptr;
    std::vector<DescSlider*> settingsSliders_;
    MenuPill* wiringMode_ = nullptr;
    juce::String carteShown_;
    nova::Pill *foldbackPill_ = nullptr, *ccPill_ = nullptr, *panicPill_ = nullptr, *cartePill_ = nullptr;
    TaperGrid* taperGrid_ = nullptr;
};
