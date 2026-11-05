#pragma once
#include "BarIcons.h"
#include "PluginProcessor.h"
#include "CombinationsPanel.h"
#include "ControlPanel.h"
#include "AudioInView.h"
#include "MixerView.h"
#include "FacadeView.h"
#include "MidiMessagePanel.h"
#include "ModView.h"
#include "RouterView.h"
#include "MidiFilterEditor.h"
#include "ScaledRoot.h"
#include "InsertOverlay.h"
#include "InstrumentPanel.h"
#include "KnobModal.h"
#include "NeptuneKnob.h"
#include "NovaKit.h"
#include <map>
#include "neptune/core/I18n.h"
#include "neptune/core/Theme.h"

// Fenêtre de sonde : paramètres en direct, et ce que l'hôte envoie (transport, notes CLAP, expressions, modulation).
class NeptuneEditor : public juce::Component, private juce::Timer
{
public:
    explicit NeptuneEditor (NeptuneProcessor&);
    ~NeptuneEditor() override { stopTimer(); root_.setLookAndFeel (nullptr); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void paintContent (juce::Graphics&); // dessiné dans la racine mise à l'échelle
    bool keyPressed (const juce::KeyPress&) override; // Ctrl/Cmd+Z : annuler ; Ctrl/Cmd+Maj+Z ou Ctrl+Y : rétablir
    // Thème, contraste ou langue changés : le cadre (NeptunePluginEditor) reconstruit l'éditeur avec la nouvelle palette ; la page courante est conservée.
    std::function<void()> onLookChanged;
    int viewIndex() const noexcept { return showMixer_ ? 0 : showProbe_ ? 5 : showFacade_ ? 2 : showMod_ ? 3 : showRouter_ ? 4 : 1; }
    void restoreView (int index) { setView (index == 0, index == 5, index == 2, index == 3, index == 4); }
    static juce::Point<int> initialSize();

private:
    void timerCallback() override;
    std::unique_ptr<MidiFilterEditor> midiFilterEditor_;
    juce::TextButton midiFilterButton_; // filtre MIDI avancé de la piste sélectionnée
    juce::TextButton globalPresetButton_;   // presets globaux (tout le mixeur)
    juce::TextButton controlButton_;    // contrôleurs MIDI (CC) : interrupteur et assignations par défaut
    juce::TextButton oscButton_;        // serveur OSC : interrupteur, réseau local, port
    juce::TextButton midiSendButton_;   // fenêtre d'envoi de messages MIDI à la main (CC, notes, SysEx...)
    juce::TextButton lookButton_;       // thème, contraste, langue
    void showLookMenu();
    std::unique_ptr<MidiMessagePanel> midiSendPanel_;
    juce::TextButton combiButton_;      // écran des combinaisons d'orgue : généraux de la banque et séquence
    std::unique_ptr<CombinationsPanel> combiPanel_;
    void openCombinations();
    void openMidiSend();
    void showControlMenu();
    void importOrgan();                                // importer un orgue GrandOrgue (.organ ou paquet .orgue)
    void chooseOrganPerspectives (const std::filesystem::path& organPath);   // orgue à plusieurs perspectives : fenêtre de choix, puis finishOrganImport
    void finishOrganImport (const std::filesystem::path& organPath, const std::vector<std::string>& excludedPerspectives);   // mémoire, lecture sur le disque, import
    void importSfz();                                  // importer un instrument SFZ : choisit le fichier, demande où le mettre, puis finishSfzImport
    void finishSfzImport (const juce::File& picked, NeptuneProcessor::SfzPlacement placement);
    void importSoundFont();
    void finishSoundFontImport (const juce::File& picked, int presetIndex, NeptuneProcessor::SfzPlacement placement);
    void showGlobalPresetMenu();                       // presets globaux : enregistrer, charger, supprimer
    void saveGlobalPreset (const juce::String& name);
    void loadGlobalPreset (const juce::File&);
    juce::File settingsDir() const { return juce::File (processor_.settingsFilePath()).getParentDirectory(); }
    void showOscMenu();
    void openMidiFilter();
    juce::Rectangle<int> rootBounds() const noexcept { return { 0, 0, lw_, lh_ }; } // taille logique (avant mise à l'échelle)
    ScaledRoot root_;
    int lw_ = 1160, lh_ = 960;
    static constexpr int headerH = 58, bottomH = 40, contentTop = 68;   // en-tête et pied NOVA (logique) ; les vues commencent à contentTop

    NeptuneProcessor& processor_;
    // Langue, thème et contraste viennent de settings.json, lus une fois au démarrage de l'instance.
    neptune::core::i18n::Translator tr_;
    neptune::core::theme::Palette palette_;
    nova::Skin novaSkin_ { palette_, neptune::core::theme::KnobStyle::Led };   // échelle 1 : la racine met déjà tout à l'échelle
    std::shared_ptr<nova::DialogLook> dialogLook_ = nova::DialogLook::acquire (palette_, neptune::core::theme::KnobStyle::Led);   // boîtes et menus hors de la fenêtre : même habillage
    nova::LookAndFeel novaLook_ { novaSkin_ };                                // habille la barre du haut (boutons, choix de piste) et les vues sans habillage propre
    juce::Colour colour (neptune::core::theme::Token t) const;
    juce::String text (std::string_view key) const { return juce::String::fromUTF8 (tr_.tr (key).data(), static_cast<int> (tr_.tr (key).size())); }
    juce::Label latencyLabel_;
    juce::Label recoveryLabel_;
    juce::Label profileLabel_;
    juce::Label noticeLabel_;                     // remplace le texte du profil quelques secondes (inserts ignorés par une scène)
    std::uint32_t lastNoticeSerial_ = 0;
    double noticeUntilMs_ = 0.0;
    IconButton profileSwitchButton_, profileLockButton_;
    void updateProfileControls();

    std::unique_ptr<KnobModal> modal_;
    IconButton undoButton_, redoButton_;
    IconButton prevStepButton_, nextStepButton_;   // pas précédent / suivant des scènes
    juce::Label stepLabel_;
    int lastStepPosition_ = -2, lastStepLength_ = -1;

    juce::TextButton voiceCountButton_; // « Voix : n / limite » ; un clic règle la limite globale de voix
    int lastVoiceCount_ = -1;
    int latencyTicks_ = 0;
    int diagH_ = 0;   // hauteur des lignes de diagnostic des pistes (chargement, activité) : les vues se placent dessous
   #ifdef NEPTUNE_STANDALONE_APP
    // Application autonome seulement : le projet est un fichier (.neptune) qu'on ouvre et qu'on enregistre (Ctrl+O, Ctrl+S, Ctrl+Maj+S).
    juce::TextButton fileButton_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    void showFileMenu();
    void openProject();
    void openProjectFile (const juce::File&);
    void saveProject (bool askName);
    void saveProjectTo (const juce::File&);
    void updateWindowTitle();
   #endif
    juce::TextButton viewButton_; // bascule entre la vue Mapping et la vue Sonde (texte de diagnostic de l'hôte)
    juce::TextButton panicButton_;
    bool showProbe_ = false;
    bool showMixer_ = true; // on arrive sur le Mixeur
    bool showFacade_ = false;
    bool showMod_ = false;
    bool showRouter_ = false;
    // Vue double : Mixeur à gauche, Façade à droite, séparé par une barre à glisser (pour celui qui construit un instrument).
    struct SplitBar : juce::Component
    {
        std::function<void()> onBegin;
        std::function<void (int)> onDrag;   // distance horizontale depuis le début du glissé
        SplitBar() { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); }
        void paint (juce::Graphics& g) override { g.setColour (juce::Colours::grey.withAlpha (0.45f)); g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (3.0f, 0.0f), 2.0f); }
        void mouseDown (const juce::MouseEvent&) override { if (onBegin) onBegin(); }
        void mouseDrag (const juce::MouseEvent& e) override { if (onDrag) onDrag (e.getDistanceFromDragStartX()); }
    };
    bool showSplit_ = false;
    double splitRatio_ = 0.55, splitStartRatio_ = 0.55;   // part de la largeur donnée au panneau de gauche
    // Les deux panneaux de la vue double, au choix : 0 Mixeur, 1 Façade, 2 Modulation, 3 Routeur
    enum SplitPane { paneMixer = 0, paneFacade, paneMod, paneRouter, paneCount };
    int splitLeft_ = paneMixer, splitRight_ = paneFacade;
    bool splitShows (int pane) const { return showSplit_ && (splitLeft_ == pane || splitRight_ == pane); }
    juce::String splitPaneName (int pane) const;
    void showSplitMenu();
    void updateSplitButtonText();
    juce::TextButton splitButton_;
    SplitBar splitBar_;
    bool consoleLocked_ = false;               // mode console actif et non déverrouillé pour cette session
    juce::TextButton consoleButton_, unlockButton_;
    void applyConsoleLock();                   // montre ou masque les commandes selon le verrou
    void showConsoleMenu();
    // Trois vues : Mixeur (pistes, bus, master), Mapping (l'instrument de la piste sélectionnée), Sonde (diagnostic de l'hôte)
    juce::TextButton mixerButton_, mappingButton_, facadeButton_, modButton_, routerButton_;
    std::unique_ptr<MixerView> mixerView_;
    std::unique_ptr<RouterView> routerView_;    // le routeur de notes (accouplements, divisions de clavier)
    std::unique_ptr<ModView> modView_;          // la matrice de modulation de la piste sélectionnée
    std::unique_ptr<FacadeView> facadeView_;   // la Façade de la piste sélectionnée
    // Vue Synthé : remplace la vue Mapping quand la piste sélectionnée est un synthé.
    std::unique_ptr<InsertOverlay> insertOverlay_;
    std::unique_ptr<InstrumentPanel> instrumentPanel_;   // éditeur NOVA du synthé ou du FM de la piste sélectionnée (page Edit)
    void updateInstrumentPanel();
    void openInsert (NeptuneProcessor::StripKind, int index, int slot);
    void closeInsert();
    bool instrumentShown_ = false, audioInShown_ = false;
    // Vue Orgue : remplace la vue Mapping quand la piste sélectionnée est de type Orgue.
    std::unique_ptr<AudioInView> audioInView_;
    bool selectedIsAudioIn() const { return processor_.trackType (processor_.selectedTrack()) == neptune::core::TrackType::AudioIn; }
    bool selectedIsOrgan() const { return processor_.trackType (processor_.selectedTrack()) == neptune::core::TrackType::Organ; }
    // Vue FM : remplace la vue Mapping quand la piste sélectionnée est de type FM.
    bool selectedIsFm() const { return processor_.trackType (processor_.selectedTrack()) == neptune::core::TrackType::Fm; }
    std::array<int, 128> synthKeyTrack_ {}; // piste sur laquelle chaque touche du clavier à l'écran a démarré
    bool selectedIsSynth() const { return processor_.trackType (processor_.selectedTrack()) == neptune::core::TrackType::Synth; }
    juce::ComboBox trackBox_;   // piste sélectionnée
    int knobTrack_ = -1;        // piste sur laquelle les potards d'instrument sont branchés
    std::uint32_t trackBoxRevision_ = 0xFFFFFFFFu;
    void setView (bool mixer, bool probe, bool facade = false, bool mod = false, bool router = false);
    void setSplit();
    void rebindKnobs();
    void updateTrackBox();
    void updateViewVisibility();
    std::unique_ptr<juce::FileChooser> chooser_, importChooser_;

    bool updatingControls_ = false;
    void openModal (neptune::core::ParamId, neptune::core::uistate::Terminal);
    void closeModal();
    void updateUndoButtons();
    juce::TextButton restoreButton_, discardButton_;
    bool recoveryShown_ = false;
    void updateRecoveryBanner();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeptuneEditor)
};

// Le cadre que l'hôte reçoit : redimensionnable, il contient l'éditeur de Neptune et le reconstruit quand thème, contraste ou langue changent
// (toutes les vues gardent leur propre copie de la palette ; reconstruire est plus sûr que de les mettre à jour une à une).
class NeptunePluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit NeptunePluginEditor (NeptuneProcessor&);
    void resized() override { if (content_ != nullptr) content_->setBounds (getLocalBounds()); }
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    bool keyPressed (const juce::KeyPress& k) override { return content_ != nullptr && content_->keyPressed (k); }

private:
    void build (int view);
    NeptuneProcessor& processor_;
    std::unique_ptr<NeptuneEditor> content_;
};
