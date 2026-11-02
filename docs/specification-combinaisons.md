# Combinaisons d'orgue (palier 7) — spécification

> **Mise à jour du 2026-11-02 : les combinaisons d'orgue sont devenues les « scènes », avec des modèles d'enregistrement.** La combinaison d'orgue décrite ci-dessous est le modèle d'origine, toujours le modèle par défaut. Voir `plan-scenes.md`.

Statut : **cœur écrit et testé** (`core/include/neptune/core/Combinations.h`, `core/src/Combinations.cpp`, `tests/test_combinations.cpp`) ; **branché** dans le plugin (2026-10-26, à valider dans Reaper) : processeur, couche de contrôle (CC, notes, OSC), boutons de la Façade. Pas encore fait : écran de gestion, changement de programme MIDI, divisionnels par piste via CC (voir « Étapes suivantes »).

Décisions de l'utilisateur (2026-10-26, par questions à choix) : voir « Décisions ». Ce qui suit les complète par des choix de détail, signalés « à confirmer » quand ils ne viennent pas de l'utilisateur.

## Vocabulaire

- **Piston** : un bouton qui rappelle une **mémoire** (une combinaison enregistrée).
- **Général** : mémoire qui règle toute la console (jeux, accouplements, transpositions).
- **Divisionnel** : mémoire propre à un clavier (une piste) ; ne règle que les jeux de ce clavier.
- **Annulation générale** (« général cancel ») : repousse tous les jeux et accouplements.
- **Séquenceur** : liste ordonnée de généraux parcourue pas à pas (« suivant », « précédent »).
- **Set** : touche d'enregistrement. On règle la console, on appuie sur Set, puis sur un piston : l'état courant y est mémorisé.

## Décisions

| Sujet | Décision |
|---|---|
| Ce qu'enregistre un général | Les **jeux** (modulateurs des Groupes du Sampler, ids `14000 + 16·t + i`), les **32 tirants du routeur** (ids `15000 + i`) et les **transpositions de règles** (ids `15100 + i`). Pas le mixeur ni les réglages sonores des pistes. |
| Ce que fait un divisionnel | Ne règle que les **jeux d'un seul clavier** (les 32 modulateurs de sa piste). Les autres claviers, les accouplements et les transpositions ne bougent pas. |
| Organisation | **Banques** de pistons numérotés, chaque mémoire peut porter un **nom**. |
| Annulation générale | **Tout repousser** : jeux à 0 %, tirants à 0, transpositions à 0. Les mémoires ne changent pas. |
| Enregistrement | **Set + piston**. |
| Séquenceur | Liste de généraux **pas à pas** : « suivant » et « précédent ». |
| Rappel | Pistons de la Façade, notes et CC MIDI, changement de programme MIDI, OSC. |
| Stockage | Dans le **projet** (`MixerState`), donc dans les presets globaux `.neptuneset`, et annulable. |

## Choix de détail (à confirmer)

- **Taille** : 10 banques de 10 pistons (numérotés 1 à 10 à l'écran). Les généraux ont leurs banques ; chaque clavier (piste) a les siennes pour ses divisionnels. La **banque courante** est unique, partagée par les généraux et les divisionnels (comme sur un orgue à banques). Stockage clairsemé : seules les mémoires enregistrées occupent de la place.
- **Set** : deux façons, qui coexistent. *Maintenu* : tant que la touche est tenue, un piston enregistre. *Armé* : un appui arme Set, le prochain piston enregistre puis Set se désarme. Enregistrer dans un piston déjà plein **écrase** (l'écran demandera confirmation ; le cœur n'en sait rien).
- **Rappel d'une mémoire vide** : ne change rien (le piston est signalé « vide »).
- **Rappel d'une mémoire** : règle les paramètres qu'elle contient. Un paramètre qui n'existe plus est ignoré. Un paramètre absent de la mémoire (ajouté depuis) n'est pas touché.
- **Séquenceur** : position de départ « avant le premier pas » ; « suivant » va au pas 1 ; aux extrémités il reste en place (pas de bouclage). Si l'on rappelle directement un général qui figure dans la liste, la position se cale sur lui. La position n'est pas enregistrée dans le projet (état de jeu).
- **Changement de programme MIDI** : programme 0 à 9 = piston 1 à 10 de la banque courante (général). Au-delà : ignoré. Un autre mapping (divisionnels, changement de banque) se décidera quand l'interface existera.
- **Jeux = modulateurs** : le mute de piste (autre représentation d'un jeu, cf. palier 7) **n'est pas** couvert pour l'instant : l'annulation générale couperait toutes les pistes, y compris des pistes qui ne sont pas des jeux. À rediscuter si besoin.
- **Annulation générale et modulateurs** : tous les modulateurs de toutes les pistes passent à 0, y compris ceux qui ne sont pas des jeux d'orgue (articulations, par exemple). À rediscuter (restreindre aux pistes d'un clavier de la Façade ?).

## Modèle de données (cœur)

`neptune::core::combi` :

- `Memory` : `used` (enregistrée), `name` (64 caractères au plus), `values` : liste de (ParamId, valeur réelle). Le contenu est la valeur réelle (pas normalisée), comme l'instantané du widget Action de la Façade.
- `Library` : mémoires des généraux (clef = banque·10 + piston), des divisionnels (clef = (piste·10 + banque)·10 + piston) et la **séquence** (liste de clefs de généraux, 64 pas au plus). `operator==`, `toJson`, `fromJson` (lecture tolérante, bornes appliquées).
- `consoleParams` / `stopParams (piste)` : les identifiants concernés, filtrés sur ceux que le jeu de paramètres connaît.
- `capture`, `recall`, `cancelAll` : fonctions pures sur un `ParameterSet`. `recall` et `cancelAll` rendent la liste des changements réellement faits (pour l'annulation et le retour visuel).
- `Controller` : état de jeu (banque courante, Set armé ou maintenu, position du séquenceur) et les actions : `general`, `divisional`, `cancel`, `sequenceNext`, `sequencePrevious`, `programChange`, changement de banque. Chaque action rend un résultat (`Recalled`, `Stored`, `Empty`, `Ignored`).

Stockage : `MixerState::combinations` (JSON `combinations`, absent si vide).

```json
"combinations": {
  "generals": [ { "bank": 0, "piston": 0, "name": "Plein-jeu", "values": [[14000, 100], [15000, 1]] } ],
  "divisionals": [ { "track": 1, "bank": 0, "piston": 2, "name": "Fonds 8'", "values": [[14016, 100]] } ],
  "sequence": [ { "bank": 0, "piston": 0 }, { "bank": 0, "piston": 1 } ]
}
```

## Branchement (2026-10-26, à valider dans Reaper)

- **Processeur** (`PluginProcessor`) : `combinations()` / `uiEditCombinations` (bibliothèque dans le projet, annulable), `uiCombiCommand (commande, piste)` (exécute une commande ; **un rappel = une seule entrée d'annulation**, les paramètres réglés passent par la pile d'annulation et préviennent l'hôte), `combiStatus`, `combiLit` (voyant d'un piston). L'état de jeu (banque, Set, séquenceur) vit dans le processeur, sur le fil de message, et n'est pas enregistré.
- **Commandes** (`combi::kCommand...`) : 1..10 piston général, 11..20 piston divisionnel, 21 annulation générale, 22 Set, 23 / 24 pas suivant / précédent, 25 / 26 banque suivante / précédente.
- **Couche de contrôle** : les 26 commandes sont des **actions** (menu « Actions > Combinaisons d'orgue » du panneau de contrôle). On les lie à une note ou un CC (pédalier, pistons physiques), à une adresse OSC ou à une macro, comme « Piste suivante » : un seul déclenchement par appui. Les divisionnels visent le clavier (la piste) **sélectionné** dans Neptune.
- **Façade** : un bouton **Action** dont le menu « Mode » propose « Combinaisons d'orgue » (`Widget::actionMode = 1 + commande`). Il agit à l'appui, s'allume quand l'état courant est celui de la mémoire (généraux et divisionnels), affiche G1..G10, D1..D10, GC, Set, Next, Prev, Bank + / - (avec la banque ou le pas courant). Pour un divisionnel, la piste est celle de la Façade (ou la piste sélectionnée pour la Façade globale). La touche **Set** de la Façade (widget Set) et celle des combinaisons sont **la même** : armer l'une arme l'autre, et elle arme aussi les pistons « instantané » existants.
- **Enregistrement** : Set puis un piston : l'état courant des généraux (jeux, tirants, transpositions) ou des jeux du clavier y est mémorisé, annulable.

## Étapes suivantes (non faites)

1. **Changement de programme MIDI** : `Controller::programChange` existe mais la couche de contrôle n'a pas de source « programme » ; à ajouter.
2. **Divisionnels de tous les claviers par CC** : aujourd'hui seulement le clavier sélectionné ; des actions par clavier (16 × 10) encombreraient le menu, à trancher.
3. **Écran de gestion** : noms des mémoires, copie, effacement, édition de la séquence (aujourd'hui la séquence ne s'édite pas depuis l'interface : la liste existe dans le projet et le cœur la parcourt, mais aucun écran ne la remplit).
4. **Barre de banque / pas** dédiée et retour visuel du Set maintenu (`Controller::setHeld` n'est pas encore relié à l'interface).
5. Mise à jour de la roadmap (palier 7) après validation dans Reaper.
