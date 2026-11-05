# Plan : timeline d'accords et arpégiateur (cahier des charges, 5 novembre 2026)

Statut (5 novembre 2026) : **phase 0 faite** (mémoire partagée, plugin d'essai, essayé dans Reaper) ; **phase 1 en cours** (Carte minimale : accords seuls, `ChordTimeline` dans le cœur, plugin `CartePlugin`). Noms provisoires : **Carte** (timeline d'accords et de tonalité) et **Arpège** (arpégiateur / séquenceur MIDI). À renommer avant la première version.

## But

1. Un plugin CLAP **Carte** que l'utilisateur met sur une piste. Il représente la timeline, suit la lecture de l'hôte, et porte des **conteneurs** (accord, tonalité, gamme, éventuellement autre chose) placés en temps et en mesures.
2. Les autres plugins de la suite peuvent **lire** cette information : Arpège d'abord, puis, si utile, Neptune, AnalogSynth, FM6Op, Orgamon.
3. Un plugin CLAP **Arpège** : arpégiateur et séquenceur de notes, qui sort des notes vers l'hôte (routées par l'hôte vers l'instrument suivant), calées sur le tempo, et qui sait tenir compte de l'accord et de la gamme.

## Ce que CLAP donne et ne donne pas

- Donne : le transport (tempo, mesure, position en temps, lecture/arrêt, boucle) ; la sortie de notes avec leur instant dans le bloc.
- Ne donne pas : les items ni la timeline de l'hôte ; aucune information d'accord ni de tonalité ; aucune communication entre plugins.
- Conséquences : Carte garde **ses propres conteneurs** (sauvés dans l'état du plugin, donc dans le projet) ; les plugins se parlent par **mémoire partagée du système**, avec un repli par notes MIDI.

## Communication : mémoire partagée du système

- Zone nommée, ouverte par chaque plugin : POSIX `shm_open` + `mmap` sous Linux (`/dev/shm`), `CreateFileMapping` nommé sous Windows. Fonctionne entre `.clap` différents et **aussi entre processus** (isolation de plugins dans l'hôte). Une variable globale ne convient pas.
- **Principe : Carte publie toute la carte** (la suite des conteneurs avec leurs positions en temps), pas « l'accord en cours ». Chaque lecteur la consulte avec **sa propre position de lecture**. Avantages : aucune dépendance à l'ordre dans lequel l'hôte traite les pistes ; anticipation possible (l'accord de la mesure suivante) ; une seule écriture quand l'utilisateur édite.
- Écriture : thread de message, à chaque édition. Lecture : thread audio, sans verrou, sans allocation, sans appel système.
- Format de la zone (v1, à figer avant le prototype) :
  - en-tête : marque, **version du format**, taille, **numéro de canal de liaison**, compteur de séquence (double tampon, aucun lecteur ne lit une carte à moitié écrite), **signe de vie** (horloge de l'écrivain, pour ignorer une carte morte après un plantage) ;
  - un tableau de conteneurs de taille fixe, avec plafond (par exemple 1 024) ;
  - un conteneur : début et longueur en temps (rationnels ou flottants à définir), type (accord, tonalité, gamme), fondamentale, qualité, extensions, note de basse, gamme.
- **Liaison** : un numéro de canal (1 à 8) choisi sur Carte et sur chaque lecteur, par défaut 1. Évite de lire la carte d'un autre projet ouvert en même temps. Un lecteur sans Carte à son canal se comporte comme sans carte.
- Fin de vie : l'écrivain libère la zone quand le dernier plugin se ferme ; au démarrage, une zone dont le signe de vie est trop vieux est ignorée puis recréée.
- Taille des données : quelques kilo-octets. Coût audio attendu : quelques lectures par bloc ; à mesurer sur le banc.

## Repli : notes MIDI

Carte peut aussi **jouer** l'accord en cours comme des notes sur une piste que l'hôte route vers Arpège. Moins riche (pas d'anticipation, pas de gamme), mais indépendant de la mémoire partagée et de l'hôte. Arpège accepte donc deux sources d'accord : la carte partagée, et les notes tenues reçues.

## Temps

- Référence commune : la **position en temps** de l'hôte (ppq), pas des échantillons ; les changements de tempo ne déplacent pas les conteneurs.
- Chaque plugin évalue la carte avec la position que l'hôte lui donne à chaque bloc. Pour l'arpégiateur, les instants des notes sont calculés en échantillons dans le bloc.
- Cas à traiter : lecture arrêtée, saut de position, boucle, changement de tempo en cours de bloc, arrêt de la lecture avec notes tenues (envoyer toutes les fins de note).

## Carte (timeline)

- Interface : une règle de temps avec tête de lecture qui suit l'hôte, zoom et défilement, une ou deux lignes de conteneurs (accords, tonalité/gamme).
- Édition : créer, déplacer, redimensionner, supprimer des conteneurs ; saisir un accord par nom (Dm7, C/E…) ou par ses notes ; copier ; annuler/rétablir (le projet a déjà `undo_`).
- Pas d'accès aux items de l'hôte. Un script Reaper pourra plus tard remplir Carte à partir des items ou des marqueurs.
- Sauvegarde dans l'état du plugin ; thème NOVA et aide intégrée comme les autres plugins.

## Arpège

- Entrée : notes tenues (l'accord est ce qu'on joue), ou accord de la carte quand rien n'est tenu (au choix).
- Motifs : montant, descendant, aller-retour, aléatoire, ordre de saisie, par accord ; nombre d'octaves ; grille (1/4 à 1/32, triolets) ; durée de note (gate) ; swing ; vélocité (fixe, motif, accent).
- Séquenceur : pas à pas (longueur, notes, vélocité, durée par pas), transposition selon l'accord ou la gamme de la carte.
- Gamme : quantifier les notes à la gamme de la carte ; options si une note sort de la gamme.
- Sortie : notes vers l'hôte avec un instant exact dans le bloc ; jamais de note bloquée (fins de note à l'arrêt, au saut, au changement de motif).
- Latence : nulle ; pas d'anticipation sur les notes entrantes (l'hôte ne le permet pas).

## Plan en phases

0. **Prototype de la mémoire partagée** (le plus risqué en premier) : un écrivain et un lecteur minimaux, Linux et Windows, un test automatique dans le cœur, mesure du coût sur le banc, essai dans Reaper avec deux pistes, avec et sans isolation du plugin.
1. **Carte minimale** : conteneurs d'accord seuls, interface simple, publication.
2. **Arpège de base** : motifs, grille, gate, notes tenues ; sortie de notes ; test à l'échantillon avec l'hôte du banc.
3. **Arpège + carte** : accord de la carte, gamme, anticipation.
4. **Intégration** dans la suite (instruments qui lisent la carte), tonalité, séquenceur pas à pas, repli par notes MIDI.

## Tests

- Cœur (Catch2) : format de la zone (écriture/lecture, double tampon, version inconnue, carte morte, plafond), logique des motifs et de la gamme, calcul des instants.
- Banc : coût audio du lecteur, sortie de notes à l'échantillon près, comportement à l'arrêt et en boucle, plusieurs lecteurs sur une carte.
- Reaper (Jean) : routage vers l'instrument suivant, isolation du plugin, plusieurs projets ouverts, sauvegarde et rechargement.

## Risques et questions ouvertes

- Isolation de plugin dans l'hôte (Reaper, Bitwig) : la mémoire partagée du système doit y survivre ; à vérifier au prototype. Environnements confinés à vérifier (bac à sable).
- Hôtes qui n'acheminent pas la sortie de notes d'un effet : Reaper est donné pour bon par Jean ; les autres ne sont pas testés.
- Choix de l'unité de temps dans la carte (flottants ou rationnels) : à fixer avant le format.
- Noms définitifs des plugins ; palette et place dans la suite.
- Versions : un changement de format après publication casse les anciens lecteurs ; prévoir la version dès le début et un comportement « format inconnu = pas de carte ».
