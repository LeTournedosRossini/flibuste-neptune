# Plan : réserve d'automation (emplacements assignables) pour tous les plugins de la suite

Rédigé le 2026-11-06 avec Jean. **Pas commencé.** Attendre son feu vert (et la fin de la première CI).

## Idée de Jean
L'hôte ne voit qu'un nombre limité de paramètres automatisables, appelés ici **emplacements** : 128 par plugin, **256 pour Neptune**. Un clic droit sur un réglage → « Automatiser » → le plugin le relie à un emplacement libre, et Reaper l'automatise. Les réglages à crans gardent leurs crans. Pas de projets existants, donc aucune compatibilité à ménager.

## État actuel (relevé dans le code)
- **Trident** : 32 « Macro » déjà faites (`core/include/neptune/core/Trident.h` : `assign`, `unassign`, `syncAssignments`; `adapter-juce/src/TridentPlugin.cpp`).
- **Neptune** : 32 macros (`control::kMacros`) dans la couche de contrôle (une macro peut piloter plusieurs réglages), menu clic droit dans `NeptuneKnob.cpp`, et un réglage `reducedHostList` (défaut : faux) qui ne déclare à l'hôte que les bandes et les macros. 3630 paramètres déclarés aujourd'hui.
- **Autres plugins** (via `ParamHost.h`) : pas de macros. Égaliseur 361 paramètres déclarés, Compresseur 87, La Flèche 49, AnalogSynth 38, FM6Op 23 (chiffres du banc).

## Règles de conception
1. **Un réglage est automatisable d'une seule façon.** Un plugin de 128 paramètres ou moins déclare ses réglages tels quels ET garde une petite réserve (16 emplacements) pour les réglages qu'il ne déclare pas (affichage, etc.). Un plugin de plus de 128 (Égaliseur, Neptune) ne déclare que sa réserve. Un réglage déjà déclaré directement n'est **pas** assignable à un emplacement (le menu dit « déjà automatisable directement »). Ça évite qu'un réglage soit piloté par deux courbes qui se battent.
2. **Un réglage ↔ au plus un emplacement** dans les plugins simples. Un emplacement pilote un seul réglage. Dans Neptune, une macro peut piloter plusieurs réglages (comportement existant, gardé) ; si deux macros visent le même réglage, le menu prévient.
3. **Sécurité contre la réassignation par erreur** (demandée par Jean) :
   - « Automatiser » prend le **premier emplacement libre** sans demander ; jamais un emplacement occupé.
   - Choisir un emplacement occupé exige une confirmation qui nomme l'ancien réglage et dit que les courbes de l'hôte sur cet emplacement piloteront désormais le nouveau.
   - Chaque emplacement peut être **verrouillé** (jamais réassigné, jamais effacé par erreur).
   - Un point numéroté sur le réglage indique qu'il est assigné (« 12 »).
4. **À l'assignation, l'emplacement prend la valeur actuelle du réglage** (pas de saut). Ensuite, les deux se suivent dans les deux sens : un mouvement du réglage à l'écran met l'emplacement à jour (utile pour l'écriture d'automation) ; un mouvement de l'emplacement par l'hôte met le réglage à jour. Garde-fou contre les boucles.
5. **Identité stable** : l'identifiant de l'emplacement k ne change jamais ; son nom affiché à l'hôte devient « 12 · Filtre ». Limite connue : l'automation suit l'emplacement, pas le réglage (Jean l'a comprise).
6. **Réglages à crans** : l'emplacement est continu pour l'hôte (0 à 1) et se cale sur les crans du réglage visé. Limite : l'hôte affiche un pourcentage et non le nom du cran.
7. **Réglages sans importance pour l'automation** (réglages d'affichage, dimensions de fenêtre) : pas assignables par défaut ; à confirmer plus bas.
8. **Enregistrement** : les assignations sont dans l'état du projet. Dans les presets : non (c'est un câblage de projet, pas un son), sauf Neptune qui garde son comportement actuel (macros du preset).
9. **Réglage « Réserve »** en haut du menu clic droit : « Automatiser ce réglage » (un clic), « Choisir l'emplacement... » (liste avec les noms), « Retirer l'automatisation ». Menu au style nova.

## Étapes
1. **Cœur** : classe `ParamReserve` (emplacements, assignation, verrou, synchro à deux sens, état JSON), tests (assigner, refuser un doublon, réassigner avec confirmation, verrou, aller-retour d'état, pas de saut, pas de boucle). Trident est refait dessus (32 → 128 emplacements).
2. **Adaptateur JUCE** : paramètres d'emplacements pour l'hôte (nom mis à jour à chaud), menu clic droit commun, point numéroté sur les potards, page de liste des emplacements (comme la page MACROS de Trident).
3. **Égaliseur** en premier (cas le plus lourd après Neptune, et réserve seule), vérifié à l'écran.
4. **Compresseur, La Flèche, AnalogSynth, FM6Op, Orgamon, Délai, Chœur, etc.** : réserve de 16 pour ce qui n'est pas déclaré.
5. **Neptune** : macros 32 → 256 (tailles des tableaux, `macroSeen_`, couche de contrôle, états), liste réduite par défaut, menu « Prochain emplacement libre » + « Choisir... » (liste groupée par 16, un menu de 256 lignes n'est pas utilisable), page de gestion des macros adaptée.
6. Compilation Linux + Windows, installation, essai dans Reaper par Jean (automatiser un réglage, rouvrir le projet, réassigner), puis CI sur les trois systèmes.

## Décisions de Jean (2026-11-06)
- 128 emplacements pour les plugins moyens, 256 pour Neptune. (On en ajoutera si besoin.)
- Réglages d'affichage : non assignables.
- La « réserve de 16 » est abandonnée : sans réglages d'affichage assignables, il ne reste rien à y mettre. Les plugins de 128 paramètres ou moins gardent leurs réglages déclarés directement, sans emplacements. Seuls Égaliseur, Neptune et Trident (dont les effets internes ne sont pas déclarés) ont une réserve d'emplacements.
- Retirer une assignation : prévu partout (menu clic droit « Retirer l'automatisation », liste des emplacements, verrou qui empêche la suppression accidentelle).
- Presets : les assignations ne sont pas enregistrées dans les presets (un preset ne doit pas recâbler les courbes d'un projet). À part : **un paramètre « Preset » automatisable** (zones discrètes pour passer d'un preset à l'autre), à étudier plus tard ; voir la discussion.
