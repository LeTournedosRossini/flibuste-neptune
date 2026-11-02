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

## Étape 2 : à faire — paramètres des inserts
Les paramètres des inserts (Digue, Compresseur, etc.) ne sont pas dans le jeu de paramètres vu par le moteur de scènes : ils vivent dans chaque insert. Il faut un identifiant composé (bande, emplacement, paramètre local) dans l'espace des `ParamId` (un bit d'indicateur, 7 bits de bande, 3 bits d'emplacement, 12 bits de paramètre local tiennent dans 32 bits), une abstraction d'accès aux valeurs (lire, écrire, défaut, bornes) au lieu d'un `ParameterSet` nu, un écrivain du processeur qui passe par la pile d'annulation des inserts, et les familles « Inserts » du modèle « Au choix » et « Tout Neptune » qui les incluent. Le bypass des inserts est déjà un paramètre hôte, donc déjà dans le modèle « Tout ».

## Étape 3 : à faire — glissement au rappel
Rappeler un mixeur en direct fait sauter les faders. Proposition : un glissement réglable (0 = instantané, valeur par défaut pour les jeux d'orgue ; 100 à 300 ms pour le mixeur) appliqué par petits pas aux valeurs continues, les valeurs à choix et les interrupteurs changeant au début. Le moteur lisse déjà les faders à l'intérieur d'un bloc ; le glissement se ferait côté processeur, par pas de 30 Hz.

## Plus tard
Commandes « Insérer le pas » et « Supprimer le pas » pour la Façade et la couche de contrôle (aujourd'hui seulement dans l'écran) ; noms de modèles personnalisés enregistrés ; sélection par piste dans « Au choix ».
