# Modèle commun des motifs et des accords : Scorsaire et la Flibuste Suite (Arpège, Carte)

Statut : **version 2, 5 novembre 2026** : proposition de la conversation neptune, **corrigée après la réponse de la conversation Scorsaire** (`Scorsair/ClaudeCodeScorsaire/scorsaire-/docs/reponse-scorsaire-modele-motifs-flibuste.md`, qui a vérifié dans son code). Les corrections ci-dessous sont intégrées ; ce qui reste **[à vérifier]** n'a pas été confirmé. Rédigé d'abord par la conversation neptune à partir de ce que fait déjà Scorsaire (`src/model/chordpatterns.js`, `strumcards.js`, `patternfromnotes.js`, `chorddegrees.js`, `chordquality.js`, lus en lecture seule). Rien n'a été modifié dans Scorsaire. À relire par la conversation Scorsaire : elle seule sait ce qui est réellement vrai de son code. Tout ce qui est marqué **[à vérifier]** est une hypothèse de ma part.

## But

Un motif dessiné dans la matrice d'**Arpège** doit pouvoir être joué par **Scorsaire**, et un motif de Scorsaire doit pouvoir jouer dans Arpège, sans perte inutile. Une grille d'accords de Scorsaire doit pouvoir remplir **Carte**, et Carte doit pouvoir dire quel motif joue à quel endroit. Il y a donc **un seul format de motif**, celui que Scorsaire a déjà, étendu de façon compatible, plus un **format d'échange de la ligne d'accords**.

## Principes

1. **Le format de Scorsaire est la référence.** On ne crée pas un second format ; on ajoute seulement des champs facultatifs.
2. **Temps en fractions exactes de ronde** `[numérateur, dénominateur]` (1/8 = `[1,8]`, un triolet de croches = `[1,12]`). Pas de flottants dans le fichier. Neptune convertit à l'exécution : 1 ronde = 4 noires (temps de l'hôte).
3. **Notes en degrés de l'accord**, jamais en hauteurs : le même motif joue sur tout accord. Un degré sans signe (`"3"`, `"7"`) est la note propre de l'accord à ce degré ; un degré avec signe (`"b9"`, `"#11"`) est cet intervalle exact.
4. **Chaque système ignore ce qu'il ne comprend pas, et le garde.** Un champ inconnu n'est jamais supprimé à la lecture : il est conservé et réécrit tel quel. Ainsi un motif qui passe de Arpège à Scorsaire puis revient ne perd rien. **Confirmé par Scorsaire** : aujourd'hui `cleanHit` (coup) et `createChordPattern` (motif) reconstruisent avec les seuls champs connus et jettent les autres, `alts` comprises. Changement proposé chez eux, en attente de l'accord de Jean : conserver `vel`, `gate` et `x` au niveau du coup, de la variante et du motif (validation : `vel` entier 1 à 127, `gate` 0,05 à 1, `x` objet JSON de taille bornée ; test d'aller-retour). Pas de changement du format de fichier `.scorsaire`.
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

- `length` : longueur **totale** du motif, sur toutes ses mesures (fraction de ronde) ; `bars` : nombre de mesures (1 à 16) ; la longueur d'**une** mesure du motif est `length / bars`. Un motif de deux mesures de 4/4 s'écrit `length: [2,1]`, `bars: 2`, et les `at` des coups sont comptés depuis le début de ces deux mesures.
- **Lecture sur une mesure du morceau** (règle de `patternHitsForMeasure` de Scorsaire, reprise à l'identique par Arpège pour que le même motif sonne pareil des deux côtés) : la mesure k du morceau joue la mesure `k modulo bars` du motif ; le motif se **répète** pour remplir la mesure (une mesure de 3/4 dans une mesure de 4/4 recommence au quatrième temps) ; un coup qui dépasse la fin de la mesure est **coupé à la barre**.
- Un coup : `at` (place), `dur` (longueur), `parts` (ce qui sonne, plusieurs parts = joués ensemble). Parts (grammaire de Scorsaire, `parsePart`) : `"B"` basse de l'accord, `"C"` accord entier, un degré de `"1"` à `"15"` avec jusqu'à deux `#` ou `b` (`"b9"`, `"#11"`), un décalage d'octave d'**un seul chiffre** (`"5+1"`, `"3-2"`), préfixe `"L"` pour la main gauche ; la main droite est l'absence de préfixe (il n'y a pas de `"R"`).
- `alts` (variantes, `chance`, `from`), `voices`, `bassDegree`, `folder`, `beats` : repris tels quels de Scorsaire. **Arpège les conserve, et ne joue pas** les `alts` dans une première version (à ajouter ensuite : une variante tirée au sort en fin de mesure sert bien un arpégiateur).

### Champs facultatifs ajoutés (propositions)

Tous sont facultatifs ; absents, le comportement actuel de Scorsaire ne change pas.

| Champ du coup | Sens | Scorsaire | Arpège |
|---|---|---|---|
| `accent` (existant, booléen) | coup renforcé | l'utilise | vélocité = vélocité de base + « force de l'accent » (réglage d'Arpège) |
| `staccato` (existant, booléen) | coup court | l'utilise | **ignoré** (par défaut) |
| `stroke` (existant) | sens du coup de médiator | l'utilise | ignoré |
| **`vel`** (nouveau, entier 1 à 127) | indication de vélocité de ce coup | conservée sans être utilisée (la carte de jeu de Scorsaire l'ignore tant qu'on n'en décide pas autrement) | l'utilise ; absente = vélocité de base (+ accent) |
| **`gate`** (nouveau, 0,05 à 1) | part de `dur` pendant laquelle la note sonne | conservée sans être utilisée | l'utilise ; absente = la durée de note réglée dans Arpège |
| **`x`** (nouveau, objet libre) | tout ce que seul un système comprend, rangé par nom de système : `{ "scorsaire": {...}, "arpege": {...} }` | lit `x.scorsaire`, garde le reste | lit `x.arpege`, garde le reste |

Règle de lecture : `vel` et `accent` peuvent coexister sur un même coup. Arpège joue `vel` s'il existe (puis ajoute la force de l'accent si `accent` est vrai), sinon vélocité de base (+ accent).

### Passages d'un système à l'autre

- **Scorsaire → Arpège** : rien de perdu. `accent` devient un surcroît de vélocité, `staccato` et `stroke` sont ignorés mais conservés (bloc `x` ou champs d'origine), les mains `L` et `R` sont ignorées (voir ci-dessous).
- **Arpège → Scorsaire** : au choix de l'utilisateur à l'import, **deux modes** :
  1. *ignorer la dynamique* : on repart à zéro, aucun accent n'est posé ; l'utilisateur les redessine ;
  2. *convertir* : une vélocité de base est choisie (celle d'Arpège, ou 80 par défaut) ; tout coup dont `vel` vaut au moins **base × 1,2** (80 → 96) devient `accent: true`. Le `vel` d'origine reste conservé dans le coup pour le retour.
  **Unités** (réponse de Scorsaire) : chez Scorsaire il n'y a pas de vélocité absolue dans un motif ; un coup normal vaut 1 et un accent ×1,25 (`accentBoost` de `audio/comping.js`), multiplié par un réglage de carte (0,5 à 1,2). Dans Arpège, la « force de l'accent » par défaut est donc **+20 sur une base de 80** (≈ ×1,25), fidèle à Scorsaire.
- **Staccato** : `staccato` ne concerne que Scorsaire. Dans Arpège, ignoré par défaut ; option possible plus tard « staccato = durée de note courte (par exemple 0,25) ».
- **Mains gauche et droite** : Arpège n'a qu'une sortie de notes. Une part `"L5"` est jouée comme `"5"` (la lettre `L` est ignorée, le décalage d'octave écrit est gardé). Les modes `voices` (`literal` / `led`) sont ignorés par Arpège.

## Résolution des degrés (qui connaît l'accord ?)

Un degré n'a de sens que par rapport à un accord. Arpège l'obtient dans cet ordre :

1. **L'accord de Carte** à la position de lecture (canal de liaison), si Carte est présente : fondamentale, basse, et les notes de l'accord. C'est la voie normale.
2. **Les notes tenues**, reconnues comme un accord (fondamentale + qualité, comme le fait déjà `parseChord` de Carte à l'envers) quand Carte est absente ou n'a rien à cet endroit.
3. **Repli par rang** quand les notes tenues ne forment pas un accord reconnu : `"1"` = note la plus grave, `"3"` = deuxième, `"5"` = troisième, `"7"` = quatrième ; les autres degrés sont considérés comme absents. (C'est ce que fait l'Arpège actuel avec les notes tenues, en plus simple.)
4. **Degré absent de l'accord** (un `"7"` sur un accord de trois notes, un `"9"` partout) : même règle que `absentDegree` de la carte de jeu de Scorsaire, qui a **deux valeurs** : `'skip'` (défaut : la part est sautée) et `'play'` (on joue la note par défaut du degré). Arpège a ce réglage, avec les mêmes noms. Un degré **signé** (`b9`, `#11`) n'est jamais absent : c'est l'intervalle exact au-dessus de la fondamentale ; un degré non signé cherche la note propre de l'accord (`chordOwnTone`, règle dans `audio/comping.js › degreeSemis`). Arpège reprend ces deux règles.

Les **qualités d'accord** : Scorsaire a déjà un dictionnaire `scorsaire-chord-qualities` (identifiant de qualité → intervalles en demi-tons, version 3), exportable en JSON. Carte a sa propre table de suffixes (`ChordTimeline.cpp`). **Proposition** : Carte lit et écrit le même dictionnaire, pour que `Dm7b5` signifie les mêmes notes partout. **Confirmé par Scorsaire** : le dictionnaire est du JSON lisible tel quel (`{ "format": "scorsaire-chord-qualities", "version": 3, "chordQualities": [...] }` ; le majeur a l'identifiant vide `""`, `intervals` en demi-tons, `aliases`, `variants`, `shown`, `enharmonic`, `gaps` sont de l'affichage que Carte peut ignorer et garder ; les versions 1 et 2 restent lisibles). **[non vérifié]** : la comparaison une à une avec la table de suffixes de Carte (`ChordTimeline.cpp`).

## Où vivent les motifs

Trois niveaux, comme dans Scorsaire, recherche dans cet ordre : **projet**, **utilisateur**, **fourni**.

- **Dans Arpège** : 16 emplacements dans l'état du plugin (donc dans le projet Reaper) ; une bibliothèque utilisateur dans le dossier de configuration (`~/.config/Neptune/motifs/*.json`, un motif par fichier, le format ci-dessus) ; les motifs fournis (montant, descendant, aller-retour... réécrits dans ce format).
- **Dans Scorsaire** : inchangé (`PID.CHORD_PATTERNS` du projet, préférences de l'appareil, motifs fournis).
- **Échange** : un fichier `.json` contenant un motif ou une liste, avec un champ `from` (`"arpege"` ou `"scorsaire"`). **Deux enveloppes, une chacune** : Arpège écrit `{ "format": "flibuste-motifs", "version": 1, "from": "arpege", "patterns": [ ... ] }` ; Scorsaire écrira `{ "format": "scorsaire-chord-patterns", "version": 1, "from": "scorsaire", ... }` (c'est sa convention `scorsaire-…`). **Les deux lisent les deux.** Aucun export de motifs n'existe aujourd'hui chez Scorsaire : rien à reprendre.

La **version** suit la règle de Scorsaire : un fichier d'une version plus récente que le logiciel est refusé, jamais de perte silencieuse.

## La matrice d'Arpège (ce qui sera construit)

Affichage du motif : colonnes = coups sur la grille (`length` découpée en 1/16, 1/8, triolets...), lignes = degrés (`B`, 1, 2, 3, 4, 5, 6, 7, puis les mêmes une octave au-dessus, plus 9, 11, 13 et les degrés signés par un menu). Une case allumée = une part ; plusieurs cases dans une colonne = coups simultanés ; chaque coup peut avoir accent, vélocité, durée de note. On peut copier un motif fourni dans la matrice et le retoucher. La matrice **lit et écrit exactement le JSON ci-dessus**.

## Carte : accords importés, motifs placés

1. **Ligne d'accords de Scorsaire → Carte.** Le format d'échange le plus simple : la liste des accords avec leur place en **fractions de ronde depuis le début** et leur nom en **identifiant de qualité de Scorsaire** :
   ```json
   { "format": "flibuste-accords", "version": 1, "timeSignature": [4, 4],
     "chords": [ { "at": [0, 1], "dur": [1, 1], "root": "D", "quality": "m7", "bass": null }, ... ] }
   ```
   **Chiffrage par mesure dès la version 1** (demandé par Scorsaire : un morceau peut changer de chiffrage) : `timeSignature` est une liste, une entrée par mesure (ou `[[mesure de départ, [n, d]], ...]`, forme exacte à fixer à l'écriture du code ; les deux côtés la décident ensemble). Positions cumulées en fractions de ronde depuis le début. Carte les range sur sa timeline (1 ronde = 4 noires de l'hôte). Nom de l'accord : `root` + `quality` ; l'écriture affichée reste le choix de Carte (lettres ou notes françaises). Carte ne sait pas aujourd'hui ouvrir de fichier ; une action « Importer » est à ajouter. **Confirmé par Scorsaire** : cet export n'existe pas encore (les accords ne sortent qu'en MusicXML) ; les ingrédients sont là (mesure et position dans la mesure de chaque symbole, chiffrage par mesure) ; écrire `flibuste-accords` est un petit travail de leur côté, l'enveloppe proposée leur convient.
2. **Ligne de motifs dans Carte.** Une plage « motif n° k » (emplacement 1 à 16 d'Arpège) sur la timeline, indépendante des accords. Arpège, relié au même canal, change de motif quand la plage change. Dans la carte partagée, un nouveau type de conteneur (« motif », numéro d'emplacement). **Le contenu du motif reste dans Arpège** (la carte ne porte que le numéro) : deux Arpège sur deux pistes peuvent donc interpréter le même numéro différemment, c'est voulu.
3. Ces ajouts, avec les **gammes** et les **tonalités** demandées par Jean, imposent un **format v2 de la carte partagée** (types : accord, tonalité, gamme, motif). Les anciens lecteurs traitent la v2 comme « pas de carte » : toute la suite se réinstalle d'un bloc.

## Réponses de la conversation Scorsaire (5 novembre 2026), résumé

1. `vel`, `gate`, `x` : aujourd'hui jetés ; changement chez eux proposé (conserver sans utiliser). 2. Idem, `cleanHit` et `createChordPattern`. 3. `absentDegree` : `'skip'` ou `'play'`. 4. Pas d'export de la piste d'accords ; `flibuste-accords` convient, chiffrage par mesure. 5. Pas d'enveloppe existante pour les motifs ; deux enveloppes, lues par les deux. 6. Dictionnaire de qualités lisible tel quel. 7. Unités différentes ; conversion par rapports (accent ×1,25 ; seuil ×1,2).

**Ce qui attend l'accord de Jean pour Scorsaire** : (1) conserver `vel`/`gate`/`x` ; (2) exporter / importer des motifs ; (3) exporter la piste d'accords. Aucun n'est fait.

## Ce qui est décidé par Jean (5 novembre 2026)

- Les temps restent en **fractions de ronde** (musical, exact pour les triolets).
- Le travail se fait **par degrés de l'accord** pour les deux outils ; le repli par rang n'est qu'un secours.
- Le **staccato** reste propre à Scorsaire (ignoré par Arpège) ; l'**accent** devient un surcroît de vélocité dans Arpège ; de Arpège vers Scorsaire, on choisit « ignorer la dynamique » ou « convertir en accents ».
- **Mains gauche et droite ignorées** dans Arpège.
- Une **ligne d'accords de Scorsaire importable dans Carte**.
