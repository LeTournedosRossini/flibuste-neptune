#pragma once
// Interface de Diapason (générateur de signaux de test et petite boîte à rythme) au style NOVA, sur le socle commun nova::EditorBase : page SIGNAL (choix du signal,
// aperçu, mode, marche / déclenchement, potards qui suivent le signal choisi), page DRUMS (réglages du séquenceur, grille de pas, réglages du son choisi) et page SETTINGS.
#include "NovaEditor.h"
#include "neptune/core/SignalGenerator.h"
#include <cstdint>
#include <functional>
#include <vector>

class NovaDiapasonEditor : public nova::EditorBase
{
public:
    NovaDiapasonEditor (ParamHost&, neptune::core::generator::GeneratorEngine&, const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle,
                        const neptune::core::i18n::Translator&, Options);
    ~NovaDiapasonEditor() override;
    void setPageForTest (int p) { setPage (p); }
    // Envoi d'un message MIDI vers la sortie de l'hôte (fourni par le processeur) ; faux si la file est pleine
    std::function<bool (const std::vector<std::uint8_t>&)> onSendMidi;
    void selectDrumForTest (int d) { selectDrum (d); }

private:
    class ChipGrid;
    class SignalView;
    class StepGrid;
    class MidiView;
    class ToolsView;

    void layout() override;
    void tick() override;
    void pageChanged() override;
    void buildSignalPage();
    void buildDrumsPage();
    void buildMidiPage();
    void buildToolsPage();
    void assignKnobs();   // les potards de la page SIGNAL suivent le signal et le mode choisis
    void placeColumns();  // les potards montrés sont alignés à gauche
    void selectDrum (int);
    void demoBeat();
    void clearPattern();

    neptune::core::generator::GeneratorEngine& engine_;

    // en commun (toutes les pages)
    nova::Panel *metersPanel_ = nullptr, *navPanel_ = nullptr;
    nova::VMeter* outMeter_ = nullptr;
    nova::Pill* panicPill_ = nullptr;

    // page SIGNAL
    nova::Panel* signalPanel_ = nullptr;
    ChipGrid *signalChips_ = nullptr, *modeChips_ = nullptr;
    SignalView* view_ = nullptr;
    nova::Pill *onPill_ = nullptr, *triggerPill_ = nullptr, *polarityPill_ = nullptr;
    nova::Text* signalHint_ = nullptr;
    struct Column { nova::Panel* card; nova::KnobCard* knob; };
    std::vector<Column> columns_;
    std::vector<int> columnPos_;   // rang de chaque potard parmi les potards montrés (-1 : caché)
    std::uint32_t knobKey_ = 0xFFFFFFFFu;

    // page OUTILS (accordeur et métronome)
    nova::Panel* toolsPanel_ = nullptr;
    ToolsView* toolsView_ = nullptr;

    // page MIDI
    nova::Panel* midiPanel_ = nullptr;
    MidiView* midiView_ = nullptr;

    // page DRUMS
    nova::Panel *drumsPanel_ = nullptr, *drumCard_ = nullptr;
    StepGrid* grid_ = nullptr;
    nova::Pill *runPill_ = nullptr, *syncPill_ = nullptr, *tempoPill_ = nullptr, *stepsPill_ = nullptr, *swingPill_ = nullptr, *drumsLevelPill_ = nullptr, *notePill_ = nullptr, *masterPill_ = nullptr;
    nova::Pill *demoPill_ = nullptr, *clearPill_ = nullptr, *mutePill_ = nullptr, *playPill_ = nullptr;
    nova::Text* drumsHint_ = nullptr;
    std::vector<nova::KnobCard*> drumKnobs_;   // niveau, accordage, durée, balance du son choisi
    int selectedDrum_ = 0;
};
