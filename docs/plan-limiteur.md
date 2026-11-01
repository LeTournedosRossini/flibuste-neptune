# Plan : limiteur brickwall, nom français à choisir (2026-11-01)

Brouillon à valider par l'utilisateur avant de coder. Sources : message du banc `echanges/messages/2026-11-01_1245_de-banc_realimit-specification-limiteur.md`, spécification `~/Bureau/DAW/docs/realimit-specification-high-quality.md`, mesures `~/Bureau/DAW/docs/realimit-mesures-2026-11-01.md`, modèle de référence `~/Bureau/DAW/tools/realimit/model_hq2.py`. Reconstruction par mesures de l'extérieur du limiteur de Reaper : aucun code de Reaper n'est lu ni copié.

## Objectif
Plugin CLAP effet : limiteur brickwall à anticipation, transparent sous le seuil (bit à bit), pour l'export et le mixage. Cible de précision : le modèle Python (27 cas sur 29 à moins de 0,05 dB du comparateur du banc). Ce n'est pas identique à ReaLimit : l'interaction des deux étages quand l'étage lent est chargé n'est pas modélisée (écart moyen de 0,06 à 0,26 dB sur de la musique dense).

## Algorithme (mode Quality, d'après la spécification)
Durées données pour une anticipation A = 10 ms (480 échantillons à 48 kHz) ; elles sont converties en échantillons selon la fréquence de la session.
1. Gain d'entrée : `u = x · 10^((plafond − seuil)/20)` (le seuil est relatif au plafond). Crête = maximum des canaux (gains liés) de `|u|`, sur les échantillons (pas de crête entre échantillons).
2. Gain nécessaire `r = min(1, 10^(plafond/20) / crête)`.
3. Étage rapide : au repos, rampe droite de 2 ms (0,2·A) de profondeur proportionnelle ; gain déjà descendu, droite recalculée à chaque pas qui rejoint le pic à venir (jusqu'à A en avant) ; sinon remontée droite de durée fixe A, pente figée au début de la remontée.
4. Étage lent en dB : monte de `4 · retour / sr` dB par pas, descend de `retour / sr`. Gain appliqué = minimum des deux, fois le gain de repos.
5. Flux continu : un retard de A sur le signal, et le gain de l'instant `i` se calcule avec les pics jusqu'à `i + A` (aucun problème de causalité). Coût à surveiller : environ 480 comparaisons par échantillon dans le cas gain descendu ; à optimiser (fenêtre glissante) si le banc mesure plus de quelques pour cent de CPU.

## Lookahead et modes
- **Bouton « High Quality »** (décision de l'utilisateur, il l'appelle « iQuality ») : un clic remet le mode Quality et l'anticipation à 10 ms, c'est-à-dire exactement le réglage mesuré de ReaLimit, sans toucher à Threshold, Ceiling ni Release. Il s'allume quand ces deux réglages sont dans cet état. À confirmer : doit-il aussi remettre les trois niveaux à leurs défauts ?
- **Réglage « Lookahead »** (1 à 10 ms, 10 par défaut) : toutes les durées sont proportionnelles (rampe 0,2·A, fenêtre et remontée A). Latence déclarée = A en échantillons (441 à 44,1 kHz pour 10 ms). À 10 ms : mesuré. En dessous : extrapolation non mesurée, annoncée comme telle dans l'aide du réglage.
- **Mode « Low Latency »** (menu à côté, à implémenter dans la première version, décision de l'utilisateur : « on verra à l'usage ») : approximation du mode Low Latency du banc : rampe de 56 échantillons à 48 kHz (1,2 ms), maintien de 485 échantillons après le premier pic d'un groupe puis remontée du mode Quality, même étage lent, latence 52 échantillons à 48 kHz. Hypothèse (non démontrée) : même mécanisme que le mode Quality avec la fenêtre de 10 ms passée de l'avant à l'après. La règle exacte du maintien pour les rafales n'est pas connue : l'aide et la documentation le disent.
- Changer d'anticipation ou de mode change la latence : l'hôte est prévenu (redémarrage demandé, comme Fournaise), avec fondu pour ne pas claquer.

## Réglages (noms d'hôte en anglais, standard des limiteurs ; seul le nom du plugin est en français)
| Nom exact | Unité | Plage | Défaut |
|---|---|---|---|
| Threshold | dB | -60 à +12 | -3 |
| Ceiling | dB | -24 à 0 | -0,3 |
| Release | dB/s | 1 à 100 (le banc mesure de 6 à 26) | 15 |
| Lookahead | ms | 1 à 10 | 10 |
| Mode | Quality / Low Latency | | Quality |

Noms décidés par l'utilisateur. Le banc a donné « Threshold -60 à +12 », « Ceiling -24 à 0 », release de 6 (position 1) à 26 (0,1) : la plage « infini » à vérifier avant de la reprendre telle quelle.

## Interface (kit NOVA, fenêtre étirable)
Une page : trois potards, anticipation, menu du mode, mètre de réduction de gain, historique de réduction (comme le compresseur), crête d'entrée et de sortie. Version de développement affichée comme les autres plugins. Pas de presets, comme Diapason.

## Tests
- Cœur : sous le seuil, sortie identique bit à bit ; plafond jamais dépassé (sur les échantillons) ; latence exacte ; résultat identique quelle que soit la taille de bloc (point de comparaison avec le modèle Python, qui est hors ligne) ; changement de fréquence d'échantillonnage.
- Banc : `compare_clone.py prepare` puis `compare` sur les 29 cas (mode Quality, anticipation 10 ms) ; cible : tous les cas non difficiles à moins de 0,05 dB. Linux puis Windows.
- Low Latency et lookahead court : le banc doit rendre des cas de référence dans ces modes avec ReaLimit (aucun n'existe dans les 29). Sans cela, ces deux options restent « non vérifiées ».

## À rendre au banc (demandé par son message)
Chemin du `.clap`, noms exacts des réglages Threshold / Ceiling / Release (unités dB, dB, dB/s), numéro du plugin dans le fichier, latence déclarée (441 à 44,1 kHz).

## Questions
1. Nom français du plugin : à choisir (propositions faites à l'utilisateur).
2. Compiler et installer comme d'habitude (Linux 3 cœurs, file `claude-build`) une fois le cœur écrit et testé ; aucun push sans votre accord.

## État au 2026-11-01 (soir)
- Cœur `LimiterModule` écrit, 11 tests (`tests/test_limiter.cpp`), 887 cas du cœur passent. Plugin `Digue.clap` compilé et installé sous Linux (`~/.clap/Digue.clap`) ; Windows non fait.
- Banc (`compare_clone.py`, mode Quality, lecture des réglages par le texte affiché) : 29 cas sur 29 dans la tolérance avec `--latency 480` (28 sur 29 avec la détection automatique, qui échoue seulement sur le sinus de 30 Hz limité dès le début). Pire écart hors cas difficiles 0,0415 dB. Threshold et Ceiling s'affichent avec deux décimales : le banc règle par le texte, et avec une seule le plafond « 0.0 » couvrait 0,05 dB.
- Low Latency et lookahead court : implémentés, testés seulement par propriété (plafond tenu, creux gardé), jamais comparés à ReaLimit. Interface jamais vue à l'écran ni dans Reaper.
- Pas dans le menu d'inserts de Neptune (avant le 2026-11-01, soir : fait ensuite, voir plus bas).

## État au 2026-11-01 (soir, 19:50)
- **Low Latency** : maintien de 480 échantillons comptés depuis le pic ; **un pic qui arrive après la fin du maintien le relance** (même si la descente est minuscule), **un pic plus profond que le creux tenu (de plus de 0,01 %) le relance aussi**, les pics égaux ne l'étendent pas. 24 cas sur 29 dans 0,05 dB sur les références Low Latency du banc (15 avant). Restent : `W1024`, `W8192`, `X_rel6`, `T_lim_f60`, `Q96`.
- Le banc (message 1930) a vérifié : 44,1 et 96 kHz (latence 52 et 113 échantillons en Low Latency, écart 0,004 dB sur impulsion isolée), Windows identique à Linux bit à bit (43 rendus), règle des suites d'impulsions : **pas de règle simple** (dépend de la position dans le flux par rapport à des blocs internes de ReaLimit, taille inconnue), à ne pas imiter.
- Méthode : comparaison hors ligne avec un petit programme (`limiter_cli`) sur le dossier `digue/refll` du dossier temporaire du banc (29 entrées et références Low Latency rendues par ReaLimit ; à redemander au banc s'il a disparu) ; le critère « 0,01 % » est empirique.

## Digue comme insert de Neptune (2026-11-01, soir)
Type d'insert `limiter` (menu des inserts, après Fournaise), éditeur `NovaDigueEditor` ouvert par `InsertOverlay` (comme Fournaise), état sauvé par le type d'insert (rien de plus). Dans un insert, la latence (441 échantillons en Quality à 44,1 kHz) n'est pas compensée entre les pistes : une annonce rose le dit (`nv.latency_insert`). Regardé à l'écran sur le Standalone sous Xvfb ; pas encore dans Reaper.

## Mode Zero Latency (2026-11-01, soir)
Demande de Jean : enregistrement en direct, latence nulle acceptée au prix d'un son un peu plus dur (il remixe ensuite en bonne qualité). Troisième choix du réglage Mode (`kZeroLatency`) : aucune anticipation (latence déclarée 0, Lookahead ignoré), gain calculé échantillon par échantillon, **attaque instantanée** : le gain tombe d'un coup à la première valeur qui dépasse le plafond (le plafond est tenu exactement sur cet échantillon), puis la réduction remonte de Release dB par seconde. Pas de référence ReaLimit (il n'a pas ce mode) : vérifié par propriétés (latence 0, plafond tenu, remontée à Release dB/s, transparence sous le plafond, identique quelle que soit la taille de bloc ; 3 tests du cœur, 893 cas passent). Conséquence sonore : un saut de gain à chaque dépassement, proche d'un écrêteur sur les pics rapides ; à écouter. Dans un insert de Neptune c'est le seul mode qui ne décale pas la piste : l'annonce rose de latence non compensée n'apparaît pas quand la latence est nulle.
