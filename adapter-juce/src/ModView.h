#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include <vector>
#include "DragValuePill.h"
#include "EnvelopeEditor.h"
#include "ParamSearchPanel.h"
#include "ModulationHost.h"
#include "neptune/core/I18n.h"
#include "neptune/core/Modulation.h"
#include "neptune/core/Theme.h"

// Onglet MOD : la matrice de modulation de la piste sélectionnée. En haut, 4 LFO et 4 enveloppes ; dessous, les routes (source -> paramètre,
// quantité signée). La modulation s'AJOUTE à la valeur réglée sans la changer (les potards montrent un anneau). Chaque changement est un
// geste annulable des réglages de bande de la piste.
class ModView : public juce::Component, private juce::Timer
{
public:
    ModView (ModulationHost&, const neptune::core::theme::Palette&, const neptune::core::i18n::Translator&);
    ~ModView() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class RouteRow;
    struct LfoRow { juce::Label label; juce::ComboBox shape; DragValuePill rate, phase; };

    void timerCallback() override;
    void reload();                // relit la matrice de la piste sélectionnée
    void commit();                // enregistre la matrice (un geste annulable)
    void rebuildRoutes();
    void selectEnvelope (int index);
    void layoutRoutes();
    void addRoute();
    void removeRoute (std::size_t index);
    void chooseDestination (std::size_t index);
    juce::String text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }
    juce::Colour colour (neptune::core::theme::Token) const;

    ModulationHost& host_;
    neptune::core::theme::Palette palette_;
    const neptune::core::i18n::Translator& tr_;
    int track_ = -1;
    neptune::core::modulation::Matrix matrix_;      // copie de travail (peut contenir des routes encore sans destination)
    neptune::core::modulation::Matrix committed_;   // ce qui a été enregistré en dernier : un écart venu d'ailleurs recharge la vue
    bool updating_ = false;
    juce::Rectangle<int> lfoPanel_, envPanel_, routesPanel_;   // conteneurs dessinés derrière les commandes

    std::array<LfoRow, neptune::core::modulation::kLfos> lfoRows_;
    std::array<juce::TextButton, neptune::core::modulation::kEnvelopes> envButtons_;   // choix de l'enveloppe montrée
    std::unique_ptr<EnvelopeEditor> envEditor_;
    int envSel_ = 0;
    juce::Label lfoCaption_, envCaption_, routesCaption_, hint_, envTag_;   // envTag_ : l'enveloppe en cours d'édition, en haut à droite
    std::array<juce::Label, 3> lfoHead_;      // légende des colonnes des LFO : forme, vitesse, phase
    std::array<juce::Label, 6> routeHead_;    // légende des colonnes des routes
    juce::TextButton addButton_;
    juce::Viewport view_;
    juce::Component routesHolder_;
    std::vector<std::unique_ptr<RouteRow>> rows_;
    std::unique_ptr<ParamSearchPanel> search_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModView)
};
