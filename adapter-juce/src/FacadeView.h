#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include "PluginProcessor.h"
#include "MidiMessagePanel.h"
#include "NeptuneKnob.h"
#include "neptune/core/Facade.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Theme.h"

// Vue Façade (spec §15) : l'interface personnalisée de la piste sélectionnée. Une image de fond et des widgets (potard, bouton,
// étiquette) liés à des paramètres existants. Deux modes : Jeu (les widgets agissent sur leur paramètre) et Édition (on ajoute,
// déplace, redimensionne, lie, supprime). Chaque modification de la mise en page est un seul geste annulable (réglages de bande
// de la piste) ; le glissement travaille sur une copie locale et ne s'enregistre qu'au relâchement.
class FacadeView : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    bool isCollapsed() const { return ! editMode_ && sideHidden_; }   // barre repliée : l'éditeur donne alors toute la largeur à la Façade
    std::function<void()> onCollapsedChanged;
    FacadeView (NeptuneProcessor&, const neptune::core::theme::Palette&, const neptune::core::i18n::Translator&,
                neptune::core::theme::KnobStyle, NeptuneKnob::OpenModal);
    ~FacadeView() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    // Glisser-déposer d'un pack de styles (.neptunestyle) : il est installé dans la bibliothèque.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // « Ajouter à la Façade » (fenêtre d'un potard) : pose un widget déjà lié au paramètre, sur la Façade de la piste sélectionnée.
    void addLinked (neptune::core::facade::WidgetType, neptune::core::ParamId);
    // Façade de la piste sélectionnée (faux) ou Façade globale (vrai).
    void setGlobal (bool global);
    bool isGlobal() const noexcept { return global_; }
    void setScopeVisible (bool visible);   // le mode console masque le choix Piste / Globale

private:
    class Canvas;
    class Picker;
    class TextPrompt;
    class HelpPanel;
    class StylePanel;
    class NotchPanel;
    class ScalePanel;
    void timerCallback() override;
    void refresh();                              // relit la Façade de la piste sélectionnée si elle a changé
    void commit (neptune::core::facade::Facade, const char* undoName);   // la page affichée
    void writeBook (neptune::core::facade::Facade book, const char* undoName);   // le livre entier (toutes les pages)
    void setPage (int index);
    void rebuildPageTabs();
    void showPageMenu();
    void renamePage();
    bool hasTarget() const noexcept { return global_ || track_ >= 0; }
    // Comme commit, après avoir rattaché les widgets `touched` à leur zone et rangé le contenu des zones.
    void commitArranged (neptune::core::facade::Facade, std::set<int> touched, const char* undoName);
    void addWidget (neptune::core::facade::WidgetType);
    void chooseImage();
    void showFitMenu();
    void openPicker (bool capture = false);
    void showAlignMenu();
    void showZoneLayoutMenu();
    void showZoneAlignMenu();
    void showZoneGapMenu();
    void applySingle (const std::function<void (neptune::core::facade::Widget&)>&, const char* undoName);
    void showKeyboardMenu (int what);
    void showLampSourceMenu();
    void showActionModeMenu();
    void showMeterSourceMenu();
    void showGridSizePanel (bool forName);      // curseur de taille du nom ou des textes, mise à jour en direct
    void showGridMenu();                       // grille de scènes : colonnes, lignes, couleur, tempérament, barre de banque
    void editSceneText (int piston);           // nom et texte en plus de la scène `piston` de la banque courante
    void openMidiEditor();
    void applyZone (const std::function<void (neptune::core::facade::Widget&)>&, const char* undoName);
    int single() const noexcept { return selection_.size() == 1 ? *selection_.begin() : 0; }   // le widget sélectionné s'il n'y en a qu'un
    void editText();
    void openHelp();
    void openNotice (const juce::String& title, const juce::String& body);
    void saveFacadeFile();
    void loadFacadeFile();
    void relocate();
    void refreshMissing();
    void openStyles();
    void openNotches();
    void openScale();
    void showStyleMenu();
    void duplicateSelected();
    void deleteSelected();
    void updateButtons();
    void pruneSelection();
    juce::String text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }
    juce::Colour colour (neptune::core::theme::Token) const;

    NeptuneProcessor& processor_;
    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    neptune::core::theme::KnobStyle knobStyle_;
    NeptuneKnob::OpenModal openModal_;

    int track_ = -1;
    std::uint32_t revision_ = 0xFFFFFFFFu;
    neptune::core::facade::Facade facade_;       // copie de travail de la PAGE affichée (celle de la piste, ou celle d'un glissement en cours)
    neptune::core::facade::Facade book_;         // le livre entier : toutes les pages de la Façade de la piste ou de la Façade globale
    bool global_ = false;                        // Façade globale (track_ vaut alors -1)
    int page_ = 0;                               // page affichée
    juce::TextButton scopeTrackButton_, scopeGlobalButton_, addPageButton_, pageMenuButton_;
    std::vector<std::unique_ptr<juce::TextButton>> pageTabs_;
    std::set<int> selection_;                    // widgets sélectionnés (mode Édition)
    int lastClicked_ = 0;                        // dernier widget cliqué : la référence du rangement en cercle
    bool editMode_ = false;
    int missing_ = 0;                            // images de la Façade introuvables (fond, styles)
    int missingTick_ = 0;

    juce::TextButton sideHideButton_, sideShowButton_, gridButton_, gridOptionsButton_, editButton_, knobButton_, buttonButton_, labelButton_, menuButton_, actionButton_, curveButton_, zoneButton_, keyboardButton_, lampButton_, lampSourceButton_, setKeyButton_, actionModeButton_, midiButton_, midiEditButton_, meterButton_, meterSourceButton_, sliderButton_, stylesButton_, styleButton_, orientButton_, notchesButton_, scaleButton_, invertButton_, saveFacadeButton_, loadFacadeButton_, relocateButton_, imageButton_, fitButton_, clearImageButton_;
    juce::TextButton linkButton_, textButton_, duplicateButton_, deleteButton_, captureButton_, alignButton_, helpButton_, sameSizeButton_, layoutButton_, zoneAlignButton_, zoneGapButton_, kbChannelButton_, kbFirstButton_, kbHoldButton_, kbWheelsButton_, kbVelocityButton_;
    std::unique_ptr<Canvas> canvas_;
    std::string liveMergeKey_;                 // non vide pendant un réglage en direct : une seule entrée d'annulation pour tout le geste
    bool lastCollapsed_ = false;
    bool sideHidden_ = false;                  // jeu : la barre d'onglets (à gauche) est repliée, la Façade prend toute la place
    juce::Rectangle<int> toolbarRect_;   // conteneur arrondi des rangées d'onglets et de boutons
    std::unique_ptr<Picker> picker_;
    std::unique_ptr<TextPrompt> prompt_;
    std::unique_ptr<HelpPanel> help_;
    std::unique_ptr<StylePanel> stylePanel_;
    std::unique_ptr<NotchPanel> notchPanel_;
    std::unique_ptr<ScalePanel> scalePanel_;
    std::unique_ptr<MidiMessagePanel> midiPanel_;
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FacadeView)
};
