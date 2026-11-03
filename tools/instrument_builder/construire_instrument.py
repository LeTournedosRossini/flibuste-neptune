#!/usr/bin/env python3
"""Construit un instrument pour le Sampler de Neptune à partir d'un dossier de samples (WAV) nommés  <instrument>_<note>_<dynamique>[_<dynamique>].wav
(ex. piano_C3_pp.wav, piano_C3_v40.wav, piano_C3_v40_p7.wav). La dynamique est dite par UN OU DEUX éléments après la note :
  v40          la vélocité envoyée à l'instrument pour enregistrer ce sample (1..127)
  p7           le numéro du palier (7e niveau de dynamique, 1 = le plus doux) ; aussi L7, palier7 ou un nombre seul (7)
  pp mp mf ff  une nuance (ppp pp p mp mf f ff fff)
Deux systèmes de niveaux, pour savoir quelles vélocités envoyer (voir la commande `plan`) :
  --plage 5:127:24     24 paliers répartis de la vélocité 5 à la vélocité 127 (noms : _v<vélocité>_p<palier>)
  --velocites 10,20,30,40,60   niveaux discrets (noms : _v<vélocité>)
Un nom peut ne porter que le palier (p7) si --plage ou --velocites est donné : la vélocité en est déduite. Sinon, la vélocité d'un sample est celle de son nom ;
sans vélocité, les couches se partagent également 1..127. La zone d'un sample couvre la moitié de l'écart avec ses voisins (la première commence à 1, la dernière finit à 127).
Écrit dans le dossier :
  <instrument>.sfz           l'instrument (zones, étendues de notes et de vélocité, gain et accord de chaque sample) ; l'import SFZ de Neptune le lit
  <instrument>-rapport.txt   ce qui a été mesuré et décidé, avec la fiabilité de chaque mesure
Puis  neptune_sampler_tool import-sfz  le convertit en instrument JSON, et  neptune_sampler_tool sweep  vérifie le résultat avec le vrai moteur.

Méthode (voir docs/construire-un-instrument.md) :
 1. Niveau : maximum de la sonie MOMENTANÉE (LUFS pondéré K, fenêtre de 400 ms, libebur128 : la mesure du compteur du Master de Neptune), cherché dans les 3 s qui
    suivent le début du son. Un silence avant ou après le sample ne change pas le résultat. Autres mesures possibles : --mesure rms | crete.
 2. Gain : chaque sample est ramené au même niveau de référence ; c'est la loi de vélocité du Sampler qui fait ensuite les nuances. Aux frontières
    entre couches, le niveau est alors continu quelle que soit la loi (aucune marche).
 3. Justesse : hauteur mesurée sur les harmoniques (médiane), écart à la note nominale ; l'accord de la zone le corrige (désactivable). Une mesure peu sûre est signalée
    et n'est pas appliquée.
Commande `plan` (sans dossier) : construire_instrument.py --plan --plage 5:127:24  (ou --velocites ...) imprime la carte palier / vélocité / suffixe de nom, et la couverture qui en résulte.
Options : --raw (aucune correction, pour comparer avant / après), --mesure lufs|rms|crete (défaut lufs), --window MS (400), --search S (3), --ref-db X, --no-tune,
--max-tune CENTS (50), --max-spread CENTS (6), --tool chemin de neptune_sampler_tool"""
import sys, os, re, math, argparse, subprocess
import numpy as np
import soundfile as sf

NOTE_NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
DYN_RANK = {"ppp": 1, "pp": 2, "p": 3, "mp": 4, "mf": 5, "f": 6, "ff": 7, "fff": 8}


def note_number(text):
    m = re.fullmatch(r"([A-Ga-g])([#b]?)(-?\d+)", text)
    if not m:
        return None
    n = NOTE_NAMES[m.group(1).upper()] + (1 if m.group(2) == "#" else -1 if m.group(2) == "b" else 0)
    return (int(m.group(3)) + 1) * 12 + n  # C4 = 60


def classify(token):
    """Un élément de dynamique : ('vel', n), ('layer', n), ('word', nuance) ou None."""
    t = token.lower()
    m = re.fullmatch(r"v(?:el)?(\d{1,3})", t)
    if m and 1 <= int(m.group(1)) <= 127:
        return ("vel", int(m.group(1)))
    m = re.fullmatch(r"(?:l|p|palier|layer)(\d{1,3})", t) or re.fullmatch(r"(\d{1,3})", t)
    if m:
        return ("layer", int(m.group(1)))
    if t in DYN_RANK:
        return ("word", t)
    return None


def parse_name(stem):
    """<instrument>_<note>_<dyn>[_<dyn>] -> (instrument, note, {'vel':..,'layer':..,'word':..}) ou None."""
    parts = stem.split("_")
    for i in range(len(parts) - 1, 0, -1):   # le dernier élément qui se lit comme une note, suivi de une ou deux dynamiques
        note = note_number(parts[i])
        rest = parts[i + 1:]
        if note is None or not (1 <= len(rest) <= 2):
            continue
        dyn = {}
        for tok in rest:
            c = classify(tok)
            if c is None or c[0] in dyn:
                dyn = None
                break
            dyn[c[0]] = c[1]
        if dyn:
            return "_".join(parts[:i]), note, dyn
    return None


def velocity_map(plage, velocites):
    """Vélocités envoyées, de la plus douce à la plus forte : liste, ou None si rien n'est demandé."""
    if plage:
        first, last, n = plage.split(":")
        first, last, n = int(first), int(last), int(n)
        if n < 1 or not (1 <= first <= last <= 127):
            sys.exit("--plage première:dernière:paliers : 1 <= première <= dernière <= 127 et au moins 1 palier")
        vals = [first] if n == 1 else [round(first + i * (last - first) / (n - 1)) for i in range(n)]
    elif velocites:
        vals = [int(v) for v in velocites.split(",")]
    else:
        return None
    if sorted(set(vals)) != vals or vals[0] < 1 or vals[-1] > 127:
        sys.exit(f"vélocités non utilisables (il en faut de distinctes, croissantes, entre 1 et 127) : {vals}")
    return vals


def print_plan(vals):
    n = len(vals)
    print(f"{n} niveau(x) de dynamique. Palier / vélocité à envoyer / suffixe de nom / zone couverte :")
    for i, v in enumerate(vals):
        lo = 1 if i == 0 else (vals[i - 1] + v) // 2 + 1
        hi = 127 if i == n - 1 else (v + vals[i + 1]) // 2
        print(f"  palier {i + 1:3d}   vélocité {v:3d}   _v{v}_p{i + 1}   zone {lo}-{hi}")
    print("Noms possibles : <instrument>_<note>_v<vélocité>.wav  ou  <instrument>_<note>_v<vélocité>_p<palier>.wav  ou, si la carte est donnée au script, <instrument>_<note>_p<palier>.wav")


def onset_index(x):
    peak = np.max(np.abs(x))
    if peak <= 0:
        return 0
    idx = np.nonzero(np.abs(x) > peak * 10 ** (-40 / 20))[0]
    return int(idx[0]) if len(idx) else 0


def find_tool(explicit):
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in [explicit, os.environ.get("NEPTUNE_SAMPLER_TOOL"),
                 os.path.join(here, "..", "..", "..", "build-core", "tools", "neptune_sampler_tool"),
                 os.path.join(here, "..", "..", "build-core", "tools", "neptune_sampler_tool")]:
        if cand and os.path.isfile(cand):
            return os.path.abspath(cand)
    sys.exit("neptune_sampler_tool introuvable : le construire (cible CMake neptune_sampler_tool) ou passer --tool")


def measure_levels(tool, path, window_ms, search_s):
    """Niveaux du sample par l'outil : dict lufs, rms_db, peak_db (maximum sur la fenêtre, dans la plage de recherche), lufs_at, onset."""
    out = subprocess.run([tool, "measure", path, "--window", str(window_ms), "--search", str(search_s)], capture_output=True, text=True, check=True).stdout
    return {k: float(v) for k, v in (line.split("=") for line in out.split())}


def estimate_cents(x, rate, onset, midi):
    """Écart de hauteur en cents (médiane sur les harmoniques) et fiabilité (dispersion en cents ; None si non mesurable)."""
    seg = x[onset + int(0.08 * rate): onset + int(0.9 * rate)]
    if len(seg) < rate * 0.3:
        return None, None
    seg = seg * np.hanning(len(seg))
    n = 1 << 19
    spec = np.abs(np.fft.rfft(seg, n))
    f_nom = 440.0 * 2 ** ((midi - 69) / 12.0)
    hz = rate / n
    estimates, weights = [], []
    for h in range(1, 9):
        fc = f_nom * h
        if fc > rate * 0.4:
            break
        lo, hi = int(fc * 2 ** (-60 / 1200) / hz), int(fc * 2 ** (60 / 1200) / hz) + 1
        k = lo + int(np.argmax(spec[lo:hi]))
        if spec[k] < np.max(spec) * 10 ** (-50 / 20) or k < 2 or k >= len(spec) - 2:
            continue
        a, b, c = np.log(spec[k - 1] + 1e-12), np.log(spec[k] + 1e-12), np.log(spec[k + 1] + 1e-12)
        d = 0.5 * (a - c) / (a - 2 * b + c)
        f = (k + d) * hz / h
        estimates.append(1200 * math.log2(f / f_nom))
        weights.append(spec[k])
    if len(estimates) < 2:
        return None, None
    est = np.array(estimates)
    return float(np.median(est)), float(np.std(est))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", nargs="?")
    ap.add_argument("--plan", action="store_true", help="imprime la carte des dynamiques demandée (--plage ou --velocites) et s'arrête")
    ap.add_argument("--plage", default=None, help="première:dernière:paliers, ex. 5:127:24")
    ap.add_argument("--velocites", default=None, help="niveaux discrets, ex. 10,20,30,40,60")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--ref-db", type=float, default=None)
    ap.add_argument("--window", type=float, default=400.0, help="fenêtre de mesure en ms (400 = sonie momentanée)")
    ap.add_argument("--search", type=float, default=3.0, help="durée (s) cherchée après le début du son")
    ap.add_argument("--mesure", choices=["lufs", "rms", "crete"], default="lufs")
    ap.add_argument("--tool", default=None)
    ap.add_argument("--no-tune", action="store_true")
    ap.add_argument("--max-tune", type=float, default=50.0)
    ap.add_argument("--max-spread", type=float, default=6.0, help="dispersion maximale (cents) pour appliquer une correction de justesse")
    args = ap.parse_args()

    vmap = velocity_map(args.plage, args.velocites)
    if args.plan:
        if vmap is None:
            sys.exit("--plan demande --plage F:L:N ou --velocites a,b,c")
        print_plan(vmap)
        return
    if not args.folder:
        sys.exit("dossier de samples manquant")
    tool = find_tool(args.tool)
    found, skipped = {}, []
    for fn in sorted(os.listdir(args.folder)):
        if not fn.lower().endswith((".wav", ".flac")):
            continue
        parsed = parse_name(os.path.splitext(fn)[0])
        if parsed is None:
            skipped.append(fn)
            continue
        inst, note, dyn = parsed
        found.setdefault(inst, {}).setdefault(note, []).append((dyn, fn))
    if not found:
        sys.exit("aucun sample au gabarit <instrument>_<note>_<nuance>.wav dans " + args.folder)

    for inst, notes in found.items():
        rows = []   # note, dyn, rank, file, level, cents, spread
        for note, items in sorted(notes.items()):
            def rank_key(it):
                d = it[0]
                return (d.get("vel", 0), d.get("layer", 0), DYN_RANK.get(d.get("word"), 0))
            items.sort(key=rank_key)
            for dyn, fn in items:
                x, rate = sf.read(os.path.join(args.folder, fn), always_2d=True)
                x = x.mean(axis=1)
                on = onset_index(x)
                meas = measure_levels(tool, os.path.join(args.folder, fn), args.window, args.search)
                lvl = {"lufs": meas["lufs"], "rms": meas["rms_db"], "crete": meas["peak_db"]}[args.mesure]
                cents, spread = estimate_cents(x, rate, on, note)
                vel = dyn.get("vel")
                if vel is None and vmap is not None and "layer" in dyn and 1 <= dyn["layer"] <= len(vmap):
                    vel = vmap[dyn["layer"] - 1]
                rows.append(dict(note=note, dyn=dyn, vel=vel, file=fn, level=lvl, cents=cents, spread=spread, meas=meas))
        top = [max((r for r in rows if r["note"] == n), key=lambda r: r["level"])["level"] for n in notes]
        ref = args.ref_db if args.ref_db is not None else float(np.median(top))

        # étendues : notes (coupure à mi-chemin entre deux notes jouées), vélocités (réparties à égalité entre les nuances de la note)
        keys = sorted(notes)
        key_ranges = {}
        for i, k in enumerate(keys):
            lo = 0 if i == 0 else (keys[i - 1] + k) // 2 + 1
            hi = 127 if i == len(keys) - 1 else (k + keys[i + 1]) // 2
            key_ranges[k] = (lo, hi)
        report = [f"Instrument {inst} : {len(rows)} samples, {len(keys)} notes. Mesure : {args.mesure} (maximum sur {args.window:.0f} ms, dans les {args.search:.1f} s après le début du son). Niveau de référence {ref:.2f}."]
        if args.raw:
            report.append("MODE BRUT : aucun gain ni accord corrigé.")
        report.append(f"{'fichier':34s} {'vél.':>5s} {'zone':>8s} {'niveau':>8s} {'LUFS':>7s} {'RMS':>7s} {'crête':>7s} {'max à':>6s} {'gain':>7s} {'écart cents':>12s} {'fiab.':>7s}  accord")
        sfz = ["// " + inst + " : construit par construire_instrument.py (gain et accord mesurés)", "<global>", "loop_mode=no_loop", ""]
        for note in keys:
            layers = [r for r in rows if r["note"] == note]
            n = len(layers)
            vels = [r["vel"] for r in layers]
            if all(v is not None for v in vels):
                order = sorted(range(n), key=lambda k: vels[k])
                layers = [layers[k] for k in order]
                vels = [vels[k] for k in order]
                if len(set(vels)) != n:
                    sys.exit(f"note {note} : deux samples ont la même vélocité {vels}")
                ranges = [(1 if i == 0 else (vels[i - 1] + vels[i]) // 2 + 1, 127 if i == n - 1 else (vels[i] + vels[i + 1]) // 2) for i in range(n)]
            else:
                ranges = [(1 + (127 * i) // n, (127 * (i + 1)) // n) for i in range(n)]
            for i, r in enumerate(layers):
                vlo, vhi = ranges[i]
                gain = 0.0 if args.raw else round(ref - r["level"], 2)
                tune, why = 0.0, "-"
                if r["cents"] is None:
                    why = "non mesurable"
                elif args.raw or args.no_tune:
                    why = f"mesuré {r['cents']:+.1f}, non appliqué"
                elif r["spread"] > args.max_spread:
                    why = f"peu sûr (dispersion {r['spread']:.1f}), non appliqué"
                elif abs(r["cents"]) > args.max_tune:
                    why = f"écart trop grand ({r['cents']:+.1f}), non appliqué"
                else:
                    tune = round(-r["cents"], 1)
                    why = f"{tune:+.1f} ct" if abs(tune) >= 1.0 else "ok"
                    if abs(tune) < 1.0:
                        tune = 0.0
                c = "-" if r["cents"] is None else f"{r['cents']:+.1f}"
                s = "-" if r["spread"] is None else f"{r['spread']:.1f}"
                m = r["meas"]
                report.append(f"{r['file']:34s} {('-' if r['vel'] is None else r['vel']):>5} {f'{vlo}-{vhi}':>8s} {r['level']:8.2f} {m['lufs']:7.2f} {m['rms_db']:7.2f} {m['peak_db']:7.2f} {m['lufs_at']:6.2f} {gain:+7.2f} {c:>12s} {s:>7s}  {why}")
                lo, hi = key_ranges[note]
                sfz.append(f"<region> sample={r['file']} pitch_keycenter={note} lokey={lo} hikey={hi} lovel={vlo} hivel={vhi} volume={gain} tune={tune}")
        for fn in skipped:
            report.append(f"IGNORÉ (nom non reconnu) : {fn}")
        with open(os.path.join(args.folder, inst + ".sfz"), "w") as f:
            f.write("\n".join(sfz) + "\n")
        with open(os.path.join(args.folder, inst + "-rapport.txt"), "w") as f:
            f.write("\n".join(report) + "\n")
        print("\n".join(report))


if __name__ == "__main__":
    main()
