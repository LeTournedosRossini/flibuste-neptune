# Construire un instrument du Sampler à partir d'un dossier de samples

Guide pour une session Claude (ou pour un humain) qui reçoit un dossier de samples et doit produire un instrument Neptune prêt à charger : zones placées, volumes enchaînés sans marche entre nuances, hauteur corrigée. Écrit le 2026-11-02 ; la méthode est **validée sur des samples synthétiques seulement** (voir « Étalonnage »).

## Outils (dans `tools/`)

| Outil | Rôle |
|---|---|
| `tools/instrument_builder/construire_instrument.py` | mesure chaque sample (niveau d'attaque, hauteur), décide gain et accord, écrit `<instrument>.sfz` et `<instrument>-rapport.txt` dans le dossier |
| `neptune_sampler_tool import-sfz in.sfz out.json [--fade N] [--linear]` | convertit le SFZ avec l'import même de Neptune (zones identiques à celles que le Sampler créerait) |
| `neptune_sampler_tool validate instrument.json` | charge l'instrument comme le plugin ; avertissements (fichier introuvable...) et trous de vélocité |
| `neptune_sampler_tool measure fichier.wav [--window MS] [--search S]` | niveau d'un sample : **maximum de la sonie momentanée** (LUFS pondéré K, 400 ms, libebur128 = le compteur du Master de Neptune), cherché dans les S secondes (3) qui suivent le début du son ; donne aussi RMS et crête sur la même fenêtre |
| `neptune_sampler_tool sweep instrument.json --note N [--step S] [--window MS] [--search S] [--mesure lufs\|rms\|crete] [--out x.wav]` | joue la note N à toutes les vélocités avec le **vrai moteur** (`SamplerVoices`), mesure le niveau (même mesure que `measure`), signale les marches de plus de 1 dB par rapport à la loi de vélocité |
| `tools/instrument_builder/generer_samples_test.py dossier` | fabrique une banque synthétique (5 notes x 4 nuances, défauts de niveau et d'accord mis exprès) pour tester |

Construction : `cmake --build build-core --target neptune_sampler_tool` (via `~/.local/bin/claude-build linux ...`). Python : numpy et soundfile suffisent.

## Gabarit de nom

`<instrument>_<note>_<nuance>.wav` ; note `C3`, `F#2`, `Db4` (C4 = 60) ; nuance `ppp pp p mp mf f ff fff` ou un numéro de couche. Un nom non reconnu est listé « IGNORÉ » dans le rapport, jamais deviné. Pour d'autres gabarits, l'import de dossier de Neptune (`ZoneImport.h`) sait lire des noms plus variés, mais il ne mesure rien : ici on produit un SFZ.

## Comment le Sampler calcule le volume (ce qui fonde la méthode)

- Chaque zone (`ZoneDescription`, `core/include/neptune/core/Instrument.h`) porte `gain_db`, `tune_cents`, `root_note`, `key_low/high`, `vel_low/high`. Chaque zone a un suivi de vélocité : le gain suit `vélocité^velExponent` (`velTrackPct` 100 % par défaut, exposant 1 par défaut).
- Niveau entendu d'une note dans une couche : `niveau_du_sample + gain_db + exposant x 20 log10(vélocité / 127)`.
- Fondu entre couches (`VelocityResponse.h`) : `velocityFadeWidth` = 0 commute franchement à la frontière ; > 0 fond sur cette largeur, en puissance constante (ou linéaire avec `--linear`). Couches de même étendue (round-robin) : jouées sans fondu.
- La vélocité qui choisit la couche est celle reçue ; la courbe de vélocité de l'instrument (`velocityCurve`) la déforme avant.

## La méthode de gain

1. Mesurer le niveau de chaque sample : **maximum de la sonie momentanée** (LUFS pondéré K, fenêtre de 400 ms), cherché dans les 3 s qui suivent le début du son (premier échantillon à moins de 40 dB sous la crête). Le début du son est détecté, donc **un silence avant ou après le sample ne change pas la mesure** (vérifié : même sample avec 1,3 s de silence avant et 2 s après = même LUFS à 0,001 près). Le rapport donne LUFS, RMS et crête côte à côte, et l'instant du maximum (`max à`) : un maximum très tardif trahit un bruit parasite. `--mesure rms|crete` pour comparer.
2. Ramener **chaque sample au même niveau de référence** (médiane des niveaux des plus fortes nuances de chaque note) : `gain_db = référence - niveau`.
3. Les nuances viennent ensuite de la loi de vélocité du Sampler. Comme tous les samples ont le même niveau, le niveau entendu vaut `référence + exposant x 20 log10(v/127)` dans **toutes** les couches : continu aux frontières, quel que soit l'exposant, et continu aussi d'une note à l'autre (mêmes références).

Ce choix **efface la différence de niveau réelle entre une nuance douce et une nuance forte** ; il ne garde que le timbre. C'est le bon point de départ pour supprimer les marches, mais la loi `exposant 1` donne 42 dB entre la vélocité 1 et 127 ; l'écart réel d'un piano entre pp et ff est différent (non mesuré ici, à établir sur de vrais samples). À régler à l'oreille avec `velExponent` / `velTrackPct` de l'instrument (voir « Étalonnage »).

## Le fondu entre couches : à vérifier avec `sweep`

Le fondu « puissance constante » suppose des couches sans rapport entre elles. Des samples de même note sont au contraire **corrélés** (mêmes harmoniques, phases voisines) : leurs amplitudes s'additionnent et le niveau gonfle d'environ 3 dB au milieu du fondu. Sur la banque synthétique : fondu de 8 en puissance constante = bosse de ~3 dB à chaque frontière (6 « marches » de ±1,1 dB repérées) ; fondu linéaire ou aucun fondu = 0 marche. Un vrai piano est entre les deux (corrélation partielle) : **ne pas décider sans `sweep` sur de vrais samples**. Le fondu adoucit en revanche le changement de timbre, que `sweep` ne mesure pas : c'est une affaire d'oreille.

## Justesse

Hauteur mesurée sur les 8 premiers harmoniques (fenêtre 80 à 900 ms après le début, FFT à 524288 points, interpolation parabolique), **médiane** des estimations, dispersion en cents comme indice de fiabilité. L'accord de la zone (`tune_cents`) est corrigé de l'écart mesuré, sauf si la dispersion dépasse 6 cents (`--max-spread`), si l'écart dépasse 50 cents (`--max-tune`) ou si la mesure est impossible : le rapport le dit et rien n'est appliqué. Limites connues : sons très graves dont le fondamental est faible, attaques bruitées, sons inharmoniques (cloches, pianos très graves : l'inharmonicité tire les harmoniques hauts vers le haut — penser à n'utiliser que les premiers harmoniques pour ces cas).

## Marche à suivre pour une session

1. Lister le dossier ; vérifier le gabarit des noms avec Jean si des fichiers sont ignorés.
2. `python3 tools/instrument_builder/construire_instrument.py <dossier> --raw` puis sans `--raw` : le brut sert de « avant ».
3. `neptune_sampler_tool import-sfz <dossier>/<nom>.sfz <dossier>/<nom>.json` (sans fondu), puis essayer `--fade 8 --linear` et `--fade 8` : comparer avec `sweep` (voir « Le fondu »).
4. `neptune_sampler_tool validate` (trous, avertissements), puis `sweep` sur **plusieurs notes**, dont des notes non échantillonnées (celles du milieu entre deux samples) : 0 marche attendue.
5. Lire le rapport : toute ligne « peu sûr », « non mesurable », « écart trop grand » est à montrer à Jean, pas à passer sous silence.
6. Livrer : le `.json` (chemins absolus des samples) et le rapport. Le JSON se charge dans le Sampler (réserve + zones) ; ne pas modifier le dossier de samples d'origine.

## Résultat sur la banque synthétique (2026-11-02)

20 samples (5 notes x 4 nuances), défauts mis exprès. Brut (`--raw`) : 3 marches de +10, +12 et +7 dB aux frontières de couches. Corrigé (LUFS momentané) : 0 marche de plus de 1 dB sur 7 notes dont deux non échantillonnées (40, 50), sans fondu ou avec fondu linéaire. Les trois désaccords posés (+18, +9, -22 cents) sont retrouvés exactement et corrigés. **Ce test ne prouve que la mécanique** : des sons synthétiques parfaitement harmoniques sont l'idéal du mesureur.

## Étalonnage à faire avec de vrais samples

Prévu avec Jean : donner un vrai piano (quelques notes x 4 nuances), faire tourner la chaîne, puis Jean juge à l'oreille et on ajuste. Ce qu'on réglera alors :
- exposant de vélocité et `velTrackPct` (dynamique réelle du piano) ;
- fenêtre et plage de recherche de la mesure (400 ms / 3 s : à comparer sur un son à attaque lente ou à très long sustain), et choix LUFS ou RMS ;
- seuils de justesse (dispersion, harmoniques utilisés) ;
- largeur du fondu ;
- ce qu'il faut mesurer en plus : boucles, silences initiaux, notes manquantes, écrêtage.
Mesurer ne remplace pas l'écoute : une marche chiffrée peut être voulue (changement de timbre), un raccord chiffré parfait peut sonner artificiel.
