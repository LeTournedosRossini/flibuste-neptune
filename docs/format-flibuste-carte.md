# Format d'échange `flibuste-carte` (version 1)

Pour échanger une **carte** entre personnes et entre logiciels : les accords, les gammes (« à partir de telle mesure, sur tant de temps »), les tonalités, et les appels de motifs, sur la même grille de mesures. Écrit par **Carte** (menu « Exporter la carte... »), lu par **Carte** (« Importer... »), prévu pour être lu et écrit par **Scorsaire**.

Rédigé le 7 novembre 2026. C'est un **JSON** en UTF-8. Les règles de Scorsaire s'appliquent : un fichier d'une version plus récente que le logiciel est **refusé** (jamais de perte silencieuse) ; ce qu'un logiciel ne connaît pas est **ignoré** sans erreur ; ce qu'il ne peut pas lire est **compté et signalé**.

## Ce qui est repris de `flibuste-accords`

Les champs `timeSignatures`, `measures`, `qualities` et `chords` sont **exactement ceux de `flibuste-accords`** (`docs/modele-motifs-commun.md`) : un fichier `flibuste-carte` est un `flibuste-accords` auquel s'ajoutent des voies. Carte lit les deux formats ; Scorsaire, qui écrit déjà `flibuste-accords`, n'a qu'à ajouter les voies qui l'intéressent.

- **Positions et durées** : fractions de **ronde** depuis le début du morceau, sous la forme `[numérateur, dénominateur]` : `"at": [3, 4]` = trois noires après le début. 1 ronde = 4 noires. Carte écrit avec une précision de 1/3840 de ronde (960 pulsations par noire).
- **`from`** (logiciel qui a écrit le fichier) et **`unrolled`** (`true` : les reprises sont déroulées, ce qui est le cas de Carte) sont ceux de `flibuste-accords`. La qualité majeure est la chaîne vide `""`, comme chez Scorsaire.
- **Notes** : `"C"`, `"F#"`, `"Bb"`, `"Ebb"` (lettres anglaises, `#` et `b`). L'écriture choisie (dièse ou bémol) est conservée.

## Le fichier

```json
{
  "format": "flibuste-carte",
  "version": 1,
  "title": "Autumn Leaves",
  "from": "Neptune Carte",
  "unrolled": true,
  "timeSignatures": [ { "fromMeasure": 0, "ts": [4, 4] }, { "fromMeasure": 8, "ts": [3, 4] } ],
  "measures": [ { "at": [0, 1], "dur": [1, 1], "ts": [4, 4] } ],
  "qualities": { "m7": [0, 3, 7, 10], "7": [0, 4, 7, 10], "maj7": [0, 4, 7, 11] },
  "chords": [ { "at": [0, 1], "dur": [1, 1], "root": "D", "quality": "m7", "bass": null } ],
  "scales": [ { "at": [0, 1], "dur": [2, 1], "root": "D", "scale": "dorian", "intervals": [0, 2, 3, 5, 7, 9, 10], "name": "Dorien" } ],
  "keys": [ { "at": [0, 1], "root": "C", "mode": "major", "fifths": 0 } ],
  "globalKey": { "root": "C", "mode": "major", "fifths": 0 },
  "patterns": [ { "at": [0, 1], "dur": [2, 1], "slot": 12, "name": "Essai 1" } ],
  "x": { "carte": { } }
}
```

Toutes les voies sont **facultatives**. Une voie absente du fichier n'est pas touchée à l'import ; une voie présente (même vide) **remplace** celle du plugin.

### `measures` et `timeSignatures`
Comme `flibuste-accords`. Les mesures donnent la règle de Carte (anacrouse, changements de chiffrage). Si `measures` manque, Carte garde les siennes.

### `chords`
Comme `flibuste-accords` : `root`, `quality` (identifiant de qualité), `bass` (ou `null`), et `nc: true` pour « pas d'accord » (un trou). `qualities` donne les intervalles de chaque identifiant ; **le dictionnaire du fichier fait foi pour ce morceau**. Carte l'écrit avec les suffixes qu'elle affiche (`m7`, `maj7`, `7`, `m7b5`...).

### `scales` : une gamme placée sur un tronçon
C'est le parallèle avec les gammes de Scorsaire (« écrire une gamme à partir de la mesure N, sur n mesures »).

| Champ | Sens |
|---|---|
| `at`, `dur` | début et durée (fractions de ronde) ; une gamme couvre le tronçon et peut chevaucher des accords ou d'autres gammes (la dernière posée l'emporte à l'import) |
| `root` | fondamentale (`"D"`, `"Bb"`) |
| `scale` | **identifiant du catalogue de gammes de Scorsaire** (`major`, `natural_minor`, `dorian`, `blues_minor`, `altered`, `whole_tone`...), facultatif si `intervals` est donné |
| `intervals` | **demi-tons depuis la fondamentale** : `[0, 2, 3, 5, 7, 9, 10]`. **Toujours écrit par Carte.** C'est lui qui fait foi : une gamme que Carte ne connaît pas par son identifiant reste jouable |
| `name` | nom affiché (facultatif) ; sinon Carte utilise l'identifiant |

Règle de lecture : si `scale` désigne une gamme que Carte connaît **et** que `intervals` correspond, Carte la range comme gamme ordinaire ; sinon elle la garde comme **gamme libre** (ses intervalles, son nom). Une entrée sans `intervals` ni identifiant connu est ignorée et comptée.

**Catalogue** : Carte embarque les **72 gammes du catalogue de Scorsaire** (`src/scales/scales.json`, identifiants, noms français et anglais, intervalles) plus le chromatique de Carte : toute gamme de Scorsaire est reconnue par son identifiant. Le fichier `core/src/ScaleCatalog.inc` est généré par `tools/gen_scales.py <scales.json>`. Les positions de manche du catalogue (Berklee / CAGED, de Jens Larsen) ne sont pas reprises : Carte ne joue que des notes. En cas de désaccord, **Scorsaire fait foi pour le catalogue** ; les `intervals` écrits dans le fichier font foi pour le morceau. Une gamme que le catalogue ne connaît pas reste une gamme libre.

### `keys` et `globalKey` : les tonalités
Un marqueur de tonalité dure jusqu'au marqueur suivant. `fifths` : **nombre de quintes de −7 à 7** (négatif = bémols), comme l'armure de Scorsaire, qui n'a pas de mode ; c'est la valeur de référence. `root` et `mode` (`"major"` ou `"minor"`) précisent la tonique et l'écriture. Carte écrit les trois. À la lecture : si `root` + `mode` et `fifths` concordent, Carte garde l'écriture de `root` ; s'ils se contredisent, `fifths` l'emporte (avec le mode du fichier s'il y en a un, majeur sinon) ; `fifths` seul suffit ; `root` + `mode` sans `fifths` suffisent aussi. Sept bémols (do bémol) n'a pas d'écriture dans Carte : il devient son enharmonique (si, cinq dièses). `globalKey` est la tonalité avant le premier marqueur (ou pour tout le morceau s'il n'y a pas de marqueur).

### `patterns` : les appels de motifs
`slot` : l'**emplacement** (1 à 16) d'un Arpège ou d'un instrument de la suite ; `name` : le nom du motif au moment de l'export, **à titre d'information** (le contenu du motif ne voyage pas dans ce fichier : il se règle dans l'Arpège de chacun, et s'échange avec le format `flibuste-motifs`). À l'import, seul `slot` compte ; si `name` diffère du motif présent dans l'emplacement, Carte le signale.

### `x` : extensions
Un objet par logiciel (`x.carte`, `x.scorsaire`) : conservé tel quel quand le logiciel le sait, ignoré sinon.

## Limites et refus
Au plus 4096 mesures, 16384 accords, 4096 gammes, 1024 tonalités, 4096 appels de motifs. Au-delà : **refus** avec un message clair, rien n'est importé. Fichier illisible, mauvais format, version plus récente : refus aussi.

## Pour Scorsaire
- **Lire** : `chords`, `measures`, `scales` (avec `intervals`) et `keys` suffisent à reconstruire les gammes et les tonalités ; `patterns` peut être ignoré.
- **Écrire** : une gamme posée par Scorsaire à partir de la mesure N pour n mesures s'écrit `{ "at", "dur", "root", "scale": <identifiant du catalogue>, "intervals": <ses intervalles> }`. Les positions se calculent depuis `measures`.
- Le même fichier est lu par `flibuste-accords` (Carte actuelle) : ses voies supplémentaires sont ignorées par un lecteur qui ne les connaît pas.
