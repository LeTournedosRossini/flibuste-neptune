#!/usr/bin/env python3
"""Régénère core/src/ScaleCatalog.inc depuis le catalogue de gammes de Scorsaire (src/scales/scales.json).

Usage : python3 -I tools/gen_scales.py <chemin/scales.json> > core/src/ScaleCatalog.inc
Les 18 premières entrées gardent l'ordre, l'identifiant enregistré et les noms historiques de Carte (les états déjà écrits restent lisibles) ;
les autres suivent dans l'ordre de classement de Scorsaire. Les positions de manche (Jens Larsen) ne sont pas reprises."""
import json, sys

LEGACY = [  # (identifiant Carte, identifiant Scorsaire, nom fr, nom en)
    ("major", "major", "majeure", "major"),
    ("minor", "natural_minor", "mineure naturelle", "natural minor"),
    ("harmonic-minor", "harmonic_minor", "mineure harmonique", "harmonic minor"),
    ("melodic-minor", "melodic_minor", "mineure mélodique", "melodic minor"),
    ("dorian", "dorian", "dorienne", "dorian"),
    ("phrygian", "phrygian", "phrygienne", "phrygian"),
    ("lydian", "lydian", "lydienne", "lydian"),
    ("mixolydian", "mixolydian", "mixolydienne", "mixolydian"),
    ("locrian", "locrian", "locrienne", "locrian"),
    ("pentatonic-major", "major_pentatonic", "pentatonique majeure", "major pentatonic"),
    ("pentatonic-minor", "minor_pentatonic", "pentatonique mineure", "minor pentatonic"),
    ("blues", "blues_minor", "blues", "blues"),
    ("whole-tone", "whole_tone", "par tons", "whole tone"),
    ("diminished-hw", "diminished_hw", "diminuée (demi-ton, ton)", "diminished (half-whole)"),
    ("diminished-wh", "diminished_wh", "diminuée (ton, demi-ton)", "diminished (whole-half)"),
    ("altered", "altered", "altérée", "altered"),
    ("lydian-dominant", "lydian_dominant", "lydienne dominante", "lydian dominant"),
    ("chromatic", "", "chromatique", "chromatic"),
]

def mask(intervals):
    r = 0
    for i in intervals:
        r |= 1 << (i % 12)
    return r

def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

def main():
    cat = json.load(open(sys.argv[1], encoding="utf-8"))
    by_id = {s["id"]: s for s in cat["scales"]}
    out = ["// Généré par tools/gen_scales.py depuis le catalogue de gammes de Scorsaire (src/scales/scales.json). Ne pas modifier à la main.",
           "// { identifiant Carte, nom français, nom anglais, notes relatives, identifiant Scorsaire }"]
    used = set()
    for cid, sid, fr, en in LEGACY:
        if sid:
            if mask(by_id[sid]["intervals"]) == 0: raise SystemExit("gamme vide : " + sid)
            rel = mask(by_id[sid]["intervals"]); used.add(sid)
        else:
            rel = 0x0FFF
        out.append("{ %s, %s, %s, 0x%03X, %s }," % (q(cid), q(fr), q(en), rel, q(sid)))
    for s in sorted(cat["scales"], key=lambda s: s["rank"]):
        if s["id"] in used: continue
        out.append("{ %s, %s, %s, 0x%03X, %s }," % (q(s["id"]), q(s["name"]["fr"][0].lower() + s["name"]["fr"][1:]), q(s["name"]["en"][0].lower() + s["name"]["en"][1:]), mask(s["intervals"]), q(s["id"])))
    print("\n".join(out))

main()
