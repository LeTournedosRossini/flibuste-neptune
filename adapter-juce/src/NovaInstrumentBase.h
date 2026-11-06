#pragma once
// Socle commun des éditeurs NOVA d'instruments à presets (AnalogSynth, FM6Op) : colonne de gauche (potards de sortie, menu des pages, bouton du clavier), page MATRICE
// (grille de presets de la banque), page SETTINGS, clavier à l'écran en bandeau masquable, menu de presets NOVA par-dessus la bibliothèque existante, et les fabriques
// de commandes (curseur, pastille de valeur, pastille à menu, bascule) qui enregistrent leur mise à jour.
// L'instrument ne passe que par `InstrumentLink` (notes, gestes d'annulation, touches tenues) : le même éditeur sert au plugin indépendant et à une piste de Neptune.
#include <functional>
#include <memory>
#include <vector>
#include "NovaEditor.h"
#include "NovaPresetFields.h"
#include "NovaPresetUi.h"
#include "NovaWidgets.h"
#include "KeyboardWheel.h"
#include "SynthKeyboard.h"

struct InstrumentLink
{
    std::function<void()> beginGesture, endGesture, panic;
    std::function<void (int key, float velocity)> noteOn;
    std::function<void (int key)> noteOff;
    std::function<std::uint64_t (int word)> heldKeys;             // touches tenues (bits 0..63 puis 64..127)
    std::function<void (float)> pitchBend;                        // molette de hauteur à l'écran, -1..+1 (plugin seul ; nul : pas de molette)
    std::function<void (float)> modWheel;                         // molette de modulation à l'écran, 0..1 (CC 1)
    std::function<juce::String()> carteText;                      // plugin seul (facultatif) : « Carte : ON · canal 1 · Dm7 » ; sans lui, pas de bouton Carte
    std::function<void (juce::Component*)> carteMenu;             // menu de Carte, ancré au bouton
    int track = 0;                                                // piste dont on règle les paramètres automatisables
    std::vector<neptune::core::ParamId> outputs;                  // potards de la colonne de gauche (volume, pan, gain, accordage)
};

class NovaInstrumentBase : public nova::EditorBase
{
public:
    NovaInstrumentBase (ParamHost&, PresetHost&, InstrumentLink, const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle, const neptune::core::i18n::Translator&,
                        Options, juce::String word);
    ~NovaInstrumentBase() override;
    bool keyPressed (const juce::KeyPress&) override;
    void showPresetMenu (juce::Component* anchor);
    void setPageForTest (int p) { setPage (p); }
    void setKeyboardShownForTest (bool shown) { keyboardShown_ = shown; applyKeyboard(); resized(); }

protected:
    // À appeler par le constructeur dérivé après avoir ajouté ses propres pages (addPage) : ajoute MATRICE et SETTINGS, la colonne de gauche, le clavier.
    void buildCommon (const char* outputHelpKey);
    void layoutCommon();                   // colonne de gauche, matrice, clavier, barre du bas (à appeler depuis layout())
    void tickCommon();                     // pastille de preset, mises à jour des commandes, clavier, matrice (à appeler depuis tick())
    void paintStatus (juce::Graphics&);
    void pageChanged() override;
    virtual void reloadDescription() = 0;  // relit la description du patch (après un chargement de preset)
    float contentH() const noexcept { return keyboardShown_ ? 512.0f : 638.0f; }   // hauteur utile de la zone de pages (maquette)
    float stretch() const noexcept { return contentH() / 512.0f; }
    float Y (float rel) const noexcept { return 72.0f + rel * stretch(); }          // ordonnée (maquette) d'une position relative à 512 de haut
    static constexpr float X0 = 204.0f;
    int matrixPage() const noexcept { return numPages() - 2; }
    int settingsPage() const noexcept { return numPages() - 1; }

    // ---- fabriques : chaque commande enregistre sa mise à jour (page -1 : toujours)
    void gestureBegin();
    void gestureEnd();
    bool dragging() const noexcept { return dragging_; }
    nova::ValueSlider* sliderFn (int page, juce::String title, double lo, double hi, double step, juce::String suffix, std::function<double()> get, std::function<void (double)> set,
                                 double def, std::function<bool()> enabled = {});
    nova::NumberPill* numberFn (int page, juce::String label, int lo, int hi, std::function<int()> get, std::function<void (int)> set, std::function<bool()> enabled = {},
                                std::function<juce::String (int)> text = {});
    nova::ChoicePill* choiceFn (int page, juce::String label, std::vector<juce::String> items, std::function<int()> get, std::function<void (int)> set, std::function<bool()> enabled = {});
    nova::Pill* toggleFn (int page, juce::String text, std::function<bool()> get, std::function<void (bool)> set, std::function<bool()> enabled = {});
    nova::KnobCard* knob (int page, neptune::core::ParamId id, juce::String title, bool compact);
    juce::String paramTitle (neptune::core::ParamId);
    void onUpdate (int page, std::function<void()> f);

    InstrumentLink link_;
    PresetHost& presets_;
    bool dragging_ = false;
    bool keyboardShown_ = true;
    std::unique_ptr<nova::PresetUi> ui_;

private:
    void applyKeyboard();
    void fillBankWithFactoryPresets();
    std::vector<std::unique_ptr<nova::ChoicePill>> choices_;
    std::vector<std::vector<std::function<void()>>> updaters_;   // index page + 1 (-1 : toujours)

    nova::Panel *outPanel_ = nullptr, *navPanel_ = nullptr;
    std::vector<nova::KnobCard*> outKnobs_;
    nova::Pill* kbPill_ = nullptr;
    nova::Pill* cartePill_ = nullptr;      // barre du bas : état de Carte et son menu (plugins seuls)
    juce::String carteShown_;
    nova::Panel* matrixPanel_ = nullptr;
    nova::PresetTiles* tiles_ = nullptr;
    nova::PresetFields* fields_ = nullptr;
    nova::Text* fieldsTitle_ = nullptr;
    nova::Pill *colsPill_ = nullptr, *rowsPill_ = nullptr, *bankPrev_ = nullptr, *bankNext_ = nullptr, *bankPill_ = nullptr, *fieldPill_ = nullptr, *fillPill_ = nullptr;
    nova::Panel* keyboardPanel_ = nullptr;
    SynthKeyboard* keyboard_ = nullptr;
    KeyboardWheel *bendWheel_ = nullptr, *modWheel_ = nullptr;   // à gauche du clavier, seulement si l'hôte les fournit
    nova::Pill *octDown_ = nullptr, *octUp_ = nullptr, *holdPill_ = nullptr, *panicPill_ = nullptr;
};
