#!/usr/bin/env python3
"""Construit un instrument pour le Sampler de Neptune à partir d'un dossier de samples (WAV) nommés  <instrument>_<note>_<nuance>.wav
(ex. piano_C3_pp.wav ; nuances reconnues : ppp pp p mp mf f ff fff, ou des numéros 1..N). Écrit dans le dossier :
  <instrument>.sfz           l'instrument (zones, étendues de notes et de vélocité, gain et accord de chaque sample) ; l'import SFZ de Neptune le lit
  <instrument>-rapport.txt   ce qui a été mesuré et décidé, avec la fiabilité de chaque mesure
Puis  neptune_sampler_tool import-sfz  le convertit en instrument JSON, et  neptune_sampler_tool sweep  vérifie le résultat avec le vrai moteur.

Méthode (voir docs/construire-un-instrument.md) :
 1. Niveau : RMS de l'attaque (par défaut les 300 ms qui suivent le début du son), en dB.
 2. Gain : chaque sample est ramené au même niveau de référence ; c'est la loi de vélocité du Sampler qui fait ensuite les nuances. Aux frontières
    entre couches, le niveau est alors continu quelle que soit la loi (aucune marche).
 3. Justesse : hauteur mesurée sur les harmoniques (médiane), écart à la note nominale ; l'accord de la zone le corrige (désactivable). Une mesure peu sûre est signalée
    et n'est pas appliquée.
Options : --raw (aucune correction, pour comparer avant / après), --ref-db X, --window MS, --no-tune, --max-tune CENTS (défaut 50)"""
import sys, os, re, math, argparse
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


def parse_name(stem):
    parts = stem.split("_")
    if len(parts) < 3:
        return None
    note = note_number(parts[-2])
    dyn = parts[-1].lower()
    if note is None or not (dyn in DYN_RANK or dyn.isdigit()):
        return None
    return "_".join(parts[:-2]), note, dyn


def onset_index(x):
    peak = np.max(np.abs(x))
    if peak <= 0:
        return 0
    idx = np.nonzero(np.abs(x) > peak * 10 ** (-40 / 20))[0]
    return int(idx[0]) if len(idx) else 0


def attack_level_db(x, rate, onset, window_ms):
    seg = x[onset: onset + max(64, int(window_ms * 0.001 * rate))]
    return 20 * math.log10(max(1e-9, float(np.sqrt(np.mean(seg ** 2)))))


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
    ap.add_argument("folder")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--ref-db", type=float, default=None)
    ap.add_argument("--window", type=float, default=300.0)
    ap.add_argument("--no-tune", action="store_true")
    ap.add_argument("--max-tune", type=float, default=50.0)
    ap.add_argument("--max-spread", type=float, default=6.0, help="dispersion maximale (cents) pour appliquer une correction de justesse")
    args = ap.parse_args()

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
            items.sort(key=lambda it: DYN_RANK.get(it[0], int(it[0]) if it[0].isdigit() else 0))
            for dyn, fn in items:
                x, rate = sf.read(os.path.join(args.folder, fn), always_2d=True)
                x = x.mean(axis=1)
                on = onset_index(x)
                lvl = attack_level_db(x, rate, on, args.window)
                cents, spread = estimate_cents(x, rate, on, note)
                rows.append(dict(note=note, dyn=dyn, file=fn, level=lvl, cents=cents, spread=spread))
        top = [max((r for r in rows if r["note"] == n), key=lambda r: r["level"])["level"] for n in notes]
        ref = args.ref_db if args.ref_db is not None else float(np.median(top))

        # étendues : notes (coupure à mi-chemin entre deux notes jouées), vélocités (réparties à égalité entre les nuances de la note)
        keys = sorted(notes)
        key_ranges = {}
        for i, k in enumerate(keys):
            lo = 0 if i == 0 else (keys[i - 1] + k) // 2 + 1
            hi = 127 if i == len(keys) - 1 else (k + keys[i + 1]) // 2
            key_ranges[k] = (lo, hi)
        report = [f"Instrument {inst} : {len(rows)} samples, {len(keys)} notes. Niveau de référence {ref:.2f} dB RMS (attaque {args.window:.0f} ms)."]
        if args.raw:
            report.append("MODE BRUT : aucun gain ni accord corrigé.")
        report.append(f"{'fichier':28s} {'niveau':>8s} {'gain':>7s} {'écart cents':>12s} {'fiab.':>7s}  accord")
        sfz = ["// " + inst + " : construit par construire_instrument.py (gain et accord mesurés)", "<global>", "loop_mode=no_loop", ""]
        for note in keys:
            layers = [r for r in rows if r["note"] == note]
            n = len(layers)
            for i, r in enumerate(layers):
                vlo = 1 + (127 * i) // n
                vhi = (127 * (i + 1)) // n
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
                report.append(f"{r['file']:28s} {r['level']:8.2f} {gain:+7.2f} {c:>12s} {s:>7s}  {why}")
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
