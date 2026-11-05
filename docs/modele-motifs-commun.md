# Modèle commun des motifs et des accords : Scorsaire et la Flibuste Suite (Arpège, Carte)

Statut : **proposition du 5 novembre 2026, rédigée par la conversation neptune** à partir de ce que fait déjà Scorsaire (`src/model/chordpatterns.js`, `strumcards.js`, `patternfromnotes.js`, `chorddegrees.js`, `chordquality.js`, lus en lecture seule). Rien n'a été modifié dans Scorsaire. À relire par la conversation Scorsaire : elle seule sait ce qui est réellement vrai de son code. Tout ce qui est marqué **[à vérifier]** est une hypothèse de ma part.

## But

Un motif dessiné dans la matrice d'**Arpège** doit pouvoir être joué par **Scorsaire**, et un motif de Scorsaire doit pouvoir jouer dans Arpège, sans perte inutile. Une grille d'accords de Scorsaire doit pouvoir remplir **Carte**, et Carte doit pouvoir dire quel motif joue à quel endroit. Il y a donc **un seul format de motif**, celui que Scorsaire a déjà, étendu de façon compatible, plus un **format d'échange de la ligne d'accords**.

## Principes

1. **Le format de Scorsaire est la référence.** On ne crée pas un second format ; on ajoute seulement des champs facultatifs.
2. **Temps en fractions exactes de ronde** `[numérateur, dénominateur]` (1/8 = `[1,8]`, un triolet de croches = `[1,12]`). Pas de flottants dans le fichier. Neptune convertit à l'exécution : 1 ronde = 4 noires (temps de l'hôte).
3. **Notes en degrés de l'accord**, jamais en hauteurs : le même motif joue sur tout accord. Un degré sans signe (`"3"`, `"7"`) est la note propre de l'accord à ce degré ; un degré avec signe (`"b9"`, `"#11"`) est cet intervalle exact.
4. **Chaque système ignore ce qu'il ne comprend pas, et le garde.** Un champ inconnu n'est jamais supprimé à la lecture : il est conservé et réécrit tel quel. Ainsi un motif qui passe de Arpège à Scorsaire puis revient ne perd rien. **[à vérifier]** : aujourd'hui `cleanHit` de Scorsaire reconstruit le coup avec les seuls champs connus, donc il jette les autres ; il faudrait qu'il copie aussi le bloc `x` décrit plus bas.
5. **Le motif dit QUAND et QUOI ; la carte de jeu dit COMMENT** (vélocités, durées, ce qu'on fait d'un degré absent). C'est déjà la séparation de Scorsaire (motif / carte `strumcards.js`). Arpège a sa propre « carte de jeu » : ses réglages (vélocité de base, force de l'accent, durée de note, swing...).

## Le motif

JSON, une valeur par motif. Les champs de Scorsaire sont inchangés :

```json
{
  "id": "arpeggio-monte",
  "name": "Arpège montant",
  "length": [1, 1],
  "bars": 1,
  "hits": [
    { "at": [0, 8], "dur": [1, 8], "parts": ["1"] },
    { "at": [1, 8], "dur": [1, 8], "parts": ["3"] },
    { "at": [2, 8], "dur": [1, 8], "parts": ["5"], "accent": true },
    { "at": [3, 8], "dur": [1, 8], "parts": ["5+1"] }
  ]
}
```

- `length` : longueur du motif (fraction de ronde) ; `bars` : nombre de mesures (1 à 16) ; la mesure k du morceau joue la mesure k modulo `bars` du motif ; un motif plus court que la mesure se répète pour la remplir. (Comme dans Scorsaire.)
- Un coup : `at` (place), `dur` (longueur), `parts` (ce qui sonne, plusieurs parts = joués ensemble). Parts : `"B"` basse de l'accord, `"C"` accord entier, `"1"`..`"13"` degré, `"b9"`/`"#11"` degré signé, `"5+1"`/`"3-1"` décalage d'octave, préfixe `"L"` main gauche (voir plus bas).
- `alts` (variantes, `chance`, `from`), `voices`, `bassDegree`, `folder`, `beats` : repris tels quels de Scorsaire. **Arpège les conserve, et ne joue pas** les `alts` dans une première version (à ajouter ensuite : une variante tirée au sort en fin de mesure sert bien un arpégiateur).

### Champs facultatifs ajoutés (propositions)

Tous sont facultatifs ; absents, le comportement actuel de Scorsaire ne change pas.

| Champ du coup | Sens | Scorsaire | Arpège |
|---|---|---|---|
| `accent` (existant, booléen) | coup renforcé | l'utilise | vélocité = vélocité de base + « force de l'accent » (réglage d'Arpège) |
| `staccato` (existant, booléen) | coup court | l'utilise | **ignoré** (par défaut) |
| `stroke` (existant) | sens du coup de médiator | l'utilise | ignoré |
| **`vel`** (nouveau, 1 à 127) | vélocité absolue de ce coup | **[à décider]** ignorée, ou lue si la carte de jeu le permet | l'utilise ; absente = vélocité de base (+ accent) |
| **`gate`** (nouveau, 0,05 à 1) | part de `dur` pendant laquelle la note sonne | **[à décider]** ignorée | l'utilise ; absente = la durée de note réglée dans Arpège |
| **`x`** (nouveau, objet libre) | tout ce que seul un système comprend, rangé par nom de système : `{ "scorsaire": {...}, "arpege": {...} }` | lit `x.scorsaire`, garde le reste | lit `x.arpege`, garde le reste |

Règle de lecture : `vel` et `accent` peuvent coexister sur un même coup. Arpège joue `vel` s'il existe (puis ajoute la force de l'accent si `accent` est vrai), sinon vélocité de base (+ accent).

### Passages d'un système à l'autre

- **Scorsaire → Arpège** : rien de perdu. `accent` devient un surcroît de vélocité, `staccato` et `stroke` sont ignorés mais conservés (bloc `x` ou champs d'origine), les mains `L` et `R` sont ignorées (voir ci-dessous).
- **Arpège → Scorsaire** : au choix de l'utilisateur à l'import, **deux modes** :
  1. *ignorer la dynamique* : on repart à zéro, aucun accent n'est posé ; l'utilisateur les redessine ;
  2. *convertir* : une vélocité de base est choisie (celle d'Arpège, ou 80 par défaut **[à décider]**) ; tout coup dont `vel` dépasse la base d'au moins un seuil (par défaut +15) devient `accent: true` ; les autres perdent leur `vel`. Le `vel` d'origine reste conservé dans `x.arpege` pour le retour.
- **Staccato** : `staccato` ne concerne que Scorsaire. Dans Arpège, ignoré par défaut ; option possible plus tard « staccato = durée de note courte (par exemple 0,25) ».
- **Mains gauche et droite** : Arpège n'a qu'une sortie de notes. Une part `"L5"` est jouée comme `"5"` (la lettre `L` est ignorée, le décalage d'octave écrit est gardé). Les modes `voices` (`literal` / `led`) sont ignorés par Arpège.

## Résolution des degrés (qui connaît l'accord ?)

Un degré n'a de sens que par rapport à un accord. Arpège l'obtient dans cet ordre :

1. **L'accord de Carte** à la position de lecture (canal de liaison), si Carte est présente : fondamentale, basse, et les notes de l'accord. C'est la voie normale.
2. **Les notes tenues**, reconnues comme un accord (fondamentale + qualité, comme le fait déjà `parseChord` de Carte à l'envers) quand Carte est absente ou n'a rien à cet endroit.
3. **Repli par rang** quand les notes tenues ne forment pas un accord reconnu : `"1"` = note la plus grave, `"3"` = deuxième, `"5"` = troisième, `"7"` = quatrième ; les autres degrés sont considérés comme absents. (C'est ce que fait l'Arpège actuel avec les notes tenues, en plus simple.)
4. **Degré absent de l'accord** (un `"7"` sur un accord de trois notes, un `"9"` partout) : même règle que `absentDegree` de la carte de jeu de Scorsaire (`'skip'` par défaut : le coup est sauté, ou la part). **[à vérifier]** les autres valeurs possibles de `absentDegree` dans `strumcards.js` ; Arpège les reprendra à l'identique.

Les **qualités d'accord** : Scorsaire a déjà un dictionnaire `scorsaire-chord-qualities` (identifiant de qualité → intervalles en demi-tons, version 3), exportable en JSON. Carte a sa propre table de suffixes (`ChordTimeline.cpp`). **Proposition** : Carte lit et écrit le même dictionnaire, pour que `Dm7b5` signifie les mêmes notes partout. **[à vérifier]** que les deux tables donnent les mêmes classes de hauteur (à comparer une à une).

## Où vivent les motifs

Trois niveaux, comme dans Scorsaire, recherche dans cet ordre : **projet**, **utilisateur**, **fourni**.

- **Dans Arpège** : 16 emplacements dans l'état du plugin (donc dans le projet Reaper) ; une bibliothèque utilisateur dans le dossier de configuration (`~/.config/Neptune/motifs/*.json`, un motif par fichier, le format ci-dessus) ; les motifs fournis (montant, descendant, aller-retour... réécrits dans ce format).
- **Dans Scorsaire** : inchangé (`PID.CHORD_PATTERNS` du projet, préférences de l'appareil, motifs fournis).
- **Échange** : un fichier `.json` contenant un motif ou une liste : `{ "format": "flibuste-motifs", "version": 1, "patterns": [ ... ] }`, avec un champ `from` (`"arpege"` ou `"scorsaire"`) pour savoir d'où il vient. Scorsaire a déjà des mécanismes d'export / import comparables (dictionnaire de qualités, percmap) : **[à vérifier]** s'il vaut mieux réutiliser son enveloppe.

La **version** suit la règle de Scorsaire : un fichier d'une version plus récente que le logiciel est refusé, jamais de perte silencieuse.

## La matrice d'Arpège (ce qui sera construit)

Affichage du motif : colonnes = coups sur la grille (`length` découpée en 1/16, 1/8, triolets...), lignes = degrés (`B`, 1, 2, 3, 4, 5, 6, 7, puis les mêmes une octave au-dessus, plus 9, 11, 13 et les degrés signés par un menu). Une case allumée = une part ; plusieurs cases dans une colonne = coups simultanés ; chaque coup peut avoir accent, vélocité, durée de note. On peut copier un motif fourni dans la matrice et le retoucher. La matrice **lit et écrit exactement le JSON ci-dessus**.

## Carte : accords importés, motifs placés

1. **Ligne d'accords de Scorsaire → Carte.** Le format d'échange le plus simple : la liste des accords avec leur place en **fractions de ronde depuis le début** et leur nom en **identifiant de qualité de Scorsaire** :
   ```json
   { "format": "flibuste-accords", "version": 1, "timeSignature": [4, 4],
     "chords": [ { "at": [0, 1], "dur": [1, 1], "root": "D", "quality": "m7", "bass": null }, ... ] }
   ```
   Carte les range sur sa timeline (1 ronde = 4 noires de l'hôte) ; une mesure qui change de chiffrage est à gérer dans le fichier (`timeSignature` peut devenir une liste par mesure). Nom de l'accord : `root` + `quality` ; l'écriture affichée reste le choix de Carte (lettres ou notes françaises). Carte ne sait pas aujourd'hui ouvrir de fichier ; une action « Importer » est à ajouter. **[à vérifier]** côté Scorsaire : l'export de la piste d'accords existe-t-il, et sous quelle forme ?
2. **Ligne de motifs dans Carte.** Une plage « motif n° k » (emplacement 1 à 16 d'Arpège) sur la timeline, indépendante des accords. Arpège, relié au même canal, change de motif quand la plage change. Dans la carte partagée, un nouveau type de conteneur (« motif », numéro d'emplacement). **Le contenu du motif reste dans Arpège** (la carte ne porte que le numéro) : deux Arpège sur deux pistes peuvent donc interpréter le même numéro différemment, c'est voulu.
3. Ces ajouts, avec les **gammes** et les **tonalités** demandées par Jean, imposent un **format v2 de la carte partagée** (types : accord, tonalité, gamme, motif). Les anciens lecteurs traitent la v2 comme « pas de carte » : toute la suite se réinstalle d'un bloc.

## Questions pour la conversation Scorsaire

1. Les champs `vel` et `gate` : Scorsaire peut-il les **conserver** sans les utiliser ? Le bloc `x` ?
2. `cleanHit` jette-t-il vraiment tout champ inconnu ? Peut-on le faire conserver `x` ?
3. Les valeurs possibles de `absentDegree`, et ce que fait Scorsaire d'un degré absent d'une qualité (`strumcards.js`, `comping.js`).
4. Existe-t-il déjà un export de la **piste d'accords** (format, unité de temps, changements de chiffrage) ? Sinon, l'enveloppe `flibuste-accords` ci-dessus convient-elle ?
5. L'enveloppe d'échange `flibuste-motifs` : faut-il plutôt reprendre celle de ses exports existants ?
6. Le dictionnaire de qualités (`CHORD_QUALITIES_FORMAT`) peut-il être lu tel quel par Carte (JSON, intervalles en demi-tons) ?
7. Le seuil d'accent à l'import d'un motif d'Arpège (+15 sur 127) et la vélocité de base (80) ont-ils du sens dans les unités de Scorsaire (ses vélocités vont de 0,1 à 1,2 dans les cartes) ?

## Ce qui est décidé par Jean (5 novembre 2026)

- Les temps restent en **fractions de ronde** (musical, exact pour les triolets).
- Le travail se fait **par degrés de l'accord** pour les deux outils ; le repli par rang n'est qu'un secours.
- Le **staccato** reste propre à Scorsaire (ignoré par Arpège) ; l'**accent** devient un surcroît de vélocité dans Arpège ; de Arpège vers Scorsaire, on choisit « ignorer la dynamique » ou « convertir en accents ».
- **Mains gauche et droite ignorées** dans Arpège.
- Une **ligne d'accords de Scorsaire importable dans Carte**.
