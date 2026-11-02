#pragma once
// Libellé d'une commande de combinaison d'orgue (voir Combinations.h) pour les menus : « Combinaison : piston général 3 ». `text` traduit une clé.
#include <functional>

#include <juce_core/juce_core.h>

#include "neptune/core/Combinations.h"

inline juce::String combiCommandText (const std::function<juce::String (const char*)>& text, int command)
{
    namespace cb = neptune::core::combi;
    const juce::String prefix = text ("combi.prefix");
    if (command >= cb::kCommandGeneral && command < cb::kCommandGeneral + cb::kFacadePistons)
        return prefix + text ("combi.general") + " " + juce::String (command - cb::kCommandGeneral + 1);
    if (command >= cb::kCommandDivisional && command < cb::kCommandDivisional + cb::kFacadePistons)
        return prefix + text ("combi.divisional") + " " + juce::String (command - cb::kCommandDivisional + 1);
    switch (command)
    {
        case cb::kCommandCancel: return prefix + text ("combi.cancel");
        case cb::kCommandSet: return prefix + text ("combi.set");
        case cb::kCommandNext: return prefix + text ("combi.next");
        case cb::kCommandPrevious: return prefix + text ("combi.previous");
        case cb::kCommandBankNext: return prefix + text ("combi.bank_next");
        default: return prefix + text ("combi.bank_previous");
    }
}
