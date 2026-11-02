# Scènes (ex-combinaisons d'orgue) : plan (2026-11-02)

Décisions de l'utilisateur (2026-11-02) : les combinaisons d'orgue deviennent des **scènes**, globales, qui peuvent mémoriser ce qu'on veut dans Neptune ; la combinaison d'orgue n'est plus qu'un **modèle d'enregistrement** prêt à l'emploi. Le modèle se choisit **au moment d'enregistrer**. Les paramètres des inserts doivent pouvoir entrer dans une scène (dès le début si faisable, sinon dans un second temps). Le stepper (séquenceur) gagne l'**insertion et la suppression à la position courante**, comme dans GrandOrgue. L'annulation générale, hors orgue, **revient aux valeurs par défaut**. Ce qui n'est pas un paramètre (routage, type de piste, choix d'instrument) reste hors scène. Les combinaisons déjà enregistrées deviennent des scènes avec le modèle « combinaison d'orgue » sans perte.

## Vocabulaire
- **Scène** (ex-général) : une mémoire (nom + liste de valeurs de paramètres) dans une banque (10 banques de 10 pistons). **Scène de piste** (ex-divisionnel) : une scène limitée aux paramètres d'une piste.
- **Modèle d'enregistrement** (`combi::CaptureSpec`) : ce que Set + piston mémorise. Enregistré dans le projet avec la bibliothèque. Chaque mémoire garde ses propres valeurs : on peut mélanger dans une banque des scènes de modèles différents, le rappel ne dépend pas du modèle courant.
  - *Combinaison d'orgue* : jeux (modulateurs des pistes), tirants et transpositions du routeur (le modèle d'origine, sélectionné par défaut ; absent du JSON, donc les anciens projets ne changent pas).
  - *Jeux d'orgue seulement* : les modulateurs.
  - *Mixeur* : faders, pans, mutes, solos, trims, envois (bandes, bus, parallèles, Master).
  - *Tout Neptune* : toutes les familles de paramètres (sauf « autre »).
  - *Au choix* : des familles de paramètres (bande, envois, inserts, oscillateurs, filtres, FM, orgue, sampler, emplacements, modulateurs, routeur) et des portées (pistes, parallèles, bus, Master, global), cochées dans un menu.
- **Annulation générale** : modèles d'orgue : tout à 0 (jeux, tirants, transpositions) comme avant ; autres modèles : les paramètres du modèle reviennent à leur valeur par défaut (celle de l'ouverture du logiciel).
- **Pas à pas** (stepper) : la liste de scènes parcourue par Suivant / Précédent. **Insérer ici** met la scène choisie juste après le pas courant (en tête s'il n'y a pas de pas courant) et en fait le pas courant ; **Supprimer le pas** retire le pas courant.

## Étape 1 : faite (2026-11-02, regardée sur le Standalone, pas dans Reaper)
- Cœur (`Combinations.h/.cpp`) : `CaptureSpec`, `captureParams`, `cancelAll` selon le modèle, `Controller::sequenceInsert` / `sequenceDeleteCurrent`, `kMaxValues` 1024 -> 8192 (un modèle « Tout » dépasse 3600 valeurs), JSON `capture`. Tests : 6 nouveaux cas (`tests/test_combinations.cpp`, « scenes: ... »), 910 cas passent.
- Écran (bouton « Scenes » / « Scènes » du pied) : choix du modèle, menu « Au choix... », **Mémoriser ici** (Set + piston sans quitter l'écran, avec confirmation avant d'écraser), onglets Scènes / Scènes de piste, compteur « Pas n / m », Insérer ici / Supprimer le pas. Textes renommés partout (EN « Scenes », FR « Scènes ») ; les étiquettes G1..G10 / D1..D10 des boutons de Façade et les commandes de la couche de contrôle (clés `combi_*`) ne changent pas.

## Étape 2 : faite (2026-11-02, compilée et testée dans le cœur ; pas regardée dans Reaper)
Les paramètres des inserts entrent dans les scènes par un identifiant composé (`combi::insertParam` : bit 31 levé, bande à plat sur 7 bits (pistes 0..15, bus 16..31, Master 32, parallèles 33..), emplacement sur 3 bits, paramètre local sur 21 bits) et une interface `combi::InsertAccess` (emplacements occupés avec leur type, lire, écrire, défaut) que le processeur implémente (`NeptuneProcessor::SceneInserts`, écritures par la pile d'annulation, une seule entrée d'historique avec le reste de la scène).
- **Quels modèles les prennent** : « Tout » et « Au choix » avec la famille Inserts cochée (et les portées voulues ; une scène de piste ne prend que les inserts de cette piste). Les modèles Mixeur et Orgue n'en prennent pas. Le bypass d'un insert est un paramètre hôte : il est déjà dans le modèle « Tout ».
- **Rappel** : la scène retient le type de module de chaque emplacement ; si un autre module (ou plus rien) s'y trouve au rappel, l'emplacement est ignoré. L'allumage du bouton (`matches`) compte de même les inserts.
- **Annulation générale** : les paramètres d'insert du modèle reviennent à leur valeur par défaut.
- **Fichier** : champ `insert_types` des mémoires (liste de paires emplacement, type) ; absent s'il n'y a pas d'insert. Les anciens projets se relisent sans perte.
- Les paramètres cachés des modules et le paramètre local 4 ne sont pas mémorisés.
- Tests : 3 nouveaux cas (« scenes: insert ... », faux accès), 913 cas passent.

## Étape 3 : faite (2026-11-02, regardée sur le Standalone : fader de 0,6 dB à -49,4 dB en passant par -25,8 dB ; pas dans Reaper)
Un réglage « Glissement » dans l'écran Scènes (Instantané, 50, 100, 200, 300, 500, 1000, 2000 ms), enregistré avec les scènes du projet (`glide_ms`, absent à 0 : les anciens projets restent instantanés). Au rappel (et à l'annulation générale), les valeurs **continues** (faders, pans, envois, réglages d'insert) glissent en ligne droite de leur valeur actuelle à la cible, par pas de 33 ms (`controlTick`) ; les interrupteurs, choix et valeurs entières (jeux d'orgue, mutes, solos) changent tout de suite. Un nouveau rappel remplace le glissement en cours du même paramètre. L'historique reçoit une entrée par paramètre (valeur de départ -> cible) comme avant ; l'hôte reçoit les changements en geste continu. Le réglage est global à la bibliothèque de scènes (pas par scène). Pas de test automatique du glissement lui-même (il vit dans le processeur) ; un test du cœur couvre le fichier.

## Plus tard
Commandes « Insérer le pas » et « Supprimer le pas » pour la Façade et la couche de contrôle (aujourd'hui seulement dans l'écran) ; noms de modèles personnalisés enregistrés ; sélection par piste dans « Au choix ».
