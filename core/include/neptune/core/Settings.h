#pragma once
// Réglages globaux de l'utilisateur (palier 0, point 5) : propres à la machine, pas au projet Reaper.
// Le cœur ne sait pas où est le dossier de configuration : l'adaptateur lui donne un chemin.
// Lecture tolérante : un fichier absent, abîmé ou plus récent ne doit jamais empêcher le plugin de démarrer.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "neptune/core/Profile.h"

namespace neptune::core::settings
{
constexpr int currentSchemaVersion = 1;

using ::neptune::core::PerformanceProfile; // défini dans Profile.h (partagé avec l'état du projet)

struct GlobalSettings
{
    std::string language = "en";  // code de langue : "en" par défaut, "fr" disponible
    std::string theme = "dark";   // "dark", "light", "hc" (haut contraste), "hammond" (bois) : identifiants de la maquette
    int contrast = 1;             // 0..4, confort visuel (sans effet sur le thème "hc") ; 1 par défaut (décision de l'utilisateur, 2026-11-02)
    PerformanceProfile profile = PerformanceProfile::Live;

    // Dossiers par défaut ; vide = emplacement par défaut choisi par l'adaptateur.
    std::string samplesDir, presetsDir, irDir, exportDir;

    int autosaveIntervalSeconds = 60; // 0 = autosave désactivé ; sinon 5..3600
    int autosaveKeepDays = 30;        // les autosaves orphelins plus vieux sont supprimés ; 1..365

    // Neptune : ne déclare à l'hôte que les paramètres de bande et le pool de macros (voir control::isBandOrMacroParam). Lu à la création de
    // l'instance (l'hôte lit la liste une fois) : un changement ne joue qu'au prochain chargement. Casse l'automation déjà enregistrée sur les autres.
    bool reducedHostList = false;

    // Neptune : ligne de diagnostic sous la version (zones, notes, voix de chaque piste Sampler). Masquée par défaut ; le chargement et les erreurs restent affichés.
    bool showDiagnostics = false;

    int mixerZoomPercent = 120;   // zoom du mixeur de Neptune, 60..160 (il ne grossit que si la fenêtre a la place)

};

std::string toJson (const GlobalSettings& s);

// Valeur invalide ou inconnue -> valeur par défaut du champ ; chaque correction ajoute un message à `warnings`.
GlobalSettings fromJson (std::string_view text, std::vector<std::string>* warnings = nullptr);

// Fichier absent -> réglages par défaut (pas d'avertissement).
GlobalSettings loadFile (const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
bool saveFile (const std::filesystem::path& path, const GlobalSettings& s, std::string* error = nullptr);
} // namespace neptune::core::settings
