# Fichiers d'exemple `flibuste-accords` (écrits par Scorsaire, 5 novembre 2026)

Chaque fichier est la piste d'accords d'un petit morceau, DÉROULÉE (ce qui se joue), au format décrit dans `docs/message-pour-flibuste-formats-accords-motifs.md`. Les mesures jouées sont dans `measures` (`source` = le numéro de la mesure écrite, à partir de 0).

| Fichier | Ce qu'il montre | Ordre joué (`source`) |
|---|---|---|
| `accords-reprise-volta.json` | une reprise avec deux voltas (1. sur la mesure 1, 2. sur la mesure 2) | 0, 1, 0, 2, 3, 4 : C G / C Am F C |
| `accords-ds-al-coda.json` | segno, « To Coda », D.S. al Coda, coda | 0, 1, 2, 3, 0, 1, 2, 4, 5 : la mesure 3 n'est jouée qu'une fois |
| `accords-anacrouse-chiffrage.json` | une anacrouse d'un temps (signature 4/4, longueur 1/4), un changement en 3/4 à la mesure 3, un accord avec basse (Am7/E), un « pas d'accord » (`nc`) | 0, 1, 2, 3, 4 |

À contrôler côté Carte : les barres de mesure viennent de `measures` (l'anacrouse est plus courte que son chiffrage) ; chaque `at` est exactement la fin de l'accord précédent ; l'entrée `nc` arrête l'accord.

## Motifs : `motifs-scorsaire.json`

Fichier `scorsaire-chord-patterns` version 1 écrit par l'export de Scorsaire : trois motifs fournis par Scorsaire (ballade, valse, arpège) et un motif « Arpège avec champs de Flibuste » qui porte `vel`, `gate` et `x` sur ses coups et sur le motif (ce que Scorsaire conserve sans l'utiliser). À contrôler côté Arpège : lire les quatre motifs ; réécrire le dernier sans rien perdre (`vel`, `gate`, `x` identiques) ; refuser une version supérieure à 1.
