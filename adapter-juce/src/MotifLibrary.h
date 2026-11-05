#pragma once
// Bibliothèque de motifs partagée : un dossier, un motif par fichier (le format d'Arpège / de Scorsaire), commun à Arpège (qui y exporte) et à Orgamon (qui y choisit).
#include <juce_core/juce_core.h>

#include <vector>

#include "neptune/core/ChordPattern.h"

namespace motifs
{
// <config>/Neptune/motifs, créé au besoin (même dossier de configuration que Neptune).
inline juce::File directory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    auto dir = base.getChildFile ("Neptune").getChildFile ("motifs");
    dir.createDirectory();
    return dir;
}

// Les fichiers .json du dossier, triés par nom.
inline juce::Array<juce::File> files()
{
    auto list = directory().findChildFiles (juce::File::findFiles, false, "*.json");
    list.sort();
    return list;
}

// Premier motif d'un fichier ; faux si le fichier n'est pas un motif valide.
inline bool read (const juce::File& f, neptune::core::pattern::Pattern& out)
{
    const auto r = neptune::core::pattern::fromJson (f.loadFileAsString().toStdString());
    if (! r.ok || r.patterns.empty()) return false;
    out = r.patterns.front();
    return true;
}

// Nom de fichier sûr pour un motif (« Valse 3-4 » -> « Valse 3-4.json »).
inline juce::File fileFor (const neptune::core::pattern::Pattern& p)
{
    auto name = juce::String (p.name.empty() ? p.id : p.name).retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_éèêàùôîçÉ");
    if (name.trim().isEmpty()) name = "motif";
    return directory().getChildFile (name.trim() + ".json");
}
} // namespace motifs
