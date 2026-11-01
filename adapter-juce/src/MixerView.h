#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <vector>
#include <functional>
#include <memory>
#include "PluginProcessor.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/Theme.h"

// Vue Mixeur : les pistes actives (jusqu'à 16, bouton « + Piste »), 16 bus et le Master (palier 1, jalon 9). En haut de chaque piste :
// le générateur de son (Sampler / Synthé) et le retrait de la piste. Chaque bande : nom, canal MIDI (pistes), routage et envois
// (fenêtre), pan, fader avec vu-mètre, mute, solo (Maj+clic : solo exclusif). Cliquer le nom sélectionne la piste : la vue Mapping
// édite alors son instrument. Les valeurs automatisables sont des paramètres (fader, pan, mute, solo, niveaux d'envoi) :
// les curseurs d'ici passent par les mêmes gestes que les potards (annulation et automation de l'hôte).
class MixerView : public juce::Component, private juce::Timer
{
public:
    MixerView (NeptuneProcessor&, const neptune::core::theme::Palette&, const neptune::core::i18n::Translator&);
    ~MixerView() override;

    std::function<void (int track)> onSelectTrack;
    // L'utilisateur veut éditer un insert (clic sur un emplacement rempli, ou module qu'il vient de poser).
    std::function<void (NeptuneProcessor::StripKind, int index, int slot)> onOpenInsert;

    // Taille de conception : en dessous, tout le mixeur est réduit proportionnellement (au lieu de déborder sur le master et le bas).
    static constexpr int designWidth = 1110, designHeight = 660;   // 16 bus + master (140) + marges des conteneurs
    void placeScaled (juce::Rectangle<int> area);
    // Zoom demandé (0,6 à 1,6 ; 1 = taille de conception) : le mixeur ne dépasse jamais la place disponible, donc il grossit seulement si la fenêtre le permet.
    void setZoom (float z);

    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh(); // noms, canaux, routage, sélection : à rappeler quand l'état du mixeur change

private:
    class Strip;
    class MasterStrip;
    class InsertList;
    int stripHeight_ = 292;
    void timerCallback() override;
    void exclusiveSolo (int kind, int index); // kind : 0 piste, 1 bus, 2 piste parallèle (pistes et parallèles forment un domaine)
    void requestTypeChange (int track, neptune::core::TrackType); // confirmation si la piste a du contenu
    void requestRemove (int track);
    void requestExportTrack (int track); // piste complète -> fichier .neptunetrack
    void requestImportTrack();           // fichier .neptunetrack -> nouvelle piste
    void requestAddParallel (int parentId); // 0..15 piste, 16.. piste parallèle
    void requestRemoveParallel (int p);
    void toggleFold (int p);
    bool isExpanded (int p) const { return expanded_[static_cast<std::size_t> (p)]; }
    void appendParallels (int parentId, std::vector<int>& out) const; // parallèles visibles, en profondeur d'abord
    void layoutTracks();
    juce::Colour colour (neptune::core::theme::Token t) const;

    NeptuneProcessor& processor_;
    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    std::array<std::unique_ptr<Strip>, neptune::core::mixer::numTracks> tracks_;
    std::array<std::unique_ptr<Strip>, neptune::core::mixer::numParallels> parallels_;
    std::array<bool, neptune::core::mixer::numParallels> expanded_ {};        // repli : les filles d'une parallèle repliée sont masquées
    std::array<bool, neptune::core::mixer::numParallels> shownParallels_ {};
    juce::Component trackRow_;      // les bandes de pistes et de pistes parallèles (défile horizontalement)
    juce::Viewport trackViewport_;
    std::array<std::unique_ptr<Strip>, neptune::core::mixer::numBuses> buses_;
    std::unique_ptr<MasterStrip> master_;
    juce::Label tracksTitle_, busesTitle_;
    juce::TextButton addTrack_, zoomOut_, zoomIn_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Label zoomLabel_;
    float zoom_ = 1.0f;
    juce::Rectangle<int> lastArea_;
    juce::Rectangle<int> tracksPanel_, busesPanel_;   // conteneurs dessinés derrière les bandes
    std::array<bool, neptune::core::mixer::numTracks> shownTracks_ {}; // pistes actives telles qu'affichées
    std::uint32_t lastRevision_ = 0xFFFFFFFFu;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
