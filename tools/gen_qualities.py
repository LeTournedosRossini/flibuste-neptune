#!/usr/bin/env python3
"""Régénère core/src/QualityCatalog.inc et core/src/QualityAliases.inc depuis le dictionnaire de qualités d'accord de Scorsaire
(docs/exemples-flibuste/scorsaire-chord-qualities.json, format `scorsaire-chord-qualities`).

Usage : python3 -I tools/gen_qualities.py <chemin/scorsaire-chord-qualities.json> core/src
Les identifiants et les intervalles (sur deux octaves, jusqu'à 23 demi-tons) sont ceux de Scorsaire ; les alias de Scorsaire sont repris, plus
les écritures que Carte connaissait déjà (EXTRA, vers les identifiants de Scorsaire). Les variantes d'affichage de Scorsaire ne sont pas reprises."""
import json, sys, os

# Écritures de Carte absentes du dictionnaire de Scorsaire (à lui transmettre pour qu'il n'y ait qu'un jeu d'alias). « 2 » et « add2 » sont retirés :
# une seconde ajoutée n'est pas une neuvième ajoutée.
EXTRA = {
    "": ["maj", "major", "majeur"],
    "m": ["min", "mi", "-", "minor", "mineur"],
    "M7": ["ma7", "Δ", "Δ" "7", "△", "△" "7"],
    "m7": ["min7", "mi7"],
    "M9": ["maj9"],
    "o": ["dim"],
    "o7": ["dim7"],
    "m7b5": ["ø", "min7b5", "m7-5"],
    "sus4": ["sus"],
    "m6": ["min6"],
    "m9": ["min9"],
    "add9": ["(add9)"],
}

def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

def main():
    d = json.load(open(sys.argv[1], encoding="utf-8"))
    if d.get("format") != "scorsaire-chord-qualities": raise SystemExit("format inattendu")
    items = d["chordQualities"]["qualities"]
    ids = set()
    quals, aliases = [], []
    seen = set()
    for x in items:
        if x["id"] in ids: raise SystemExit("identifiant en double : " + x["id"])
        ids.add(x["id"])
        mask = 0
        for i in x["intervals"]:
            if not 0 <= i < 24: raise SystemExit("intervalle hors limites : %s %d" % (x["id"], i))
            mask |= 1 << i
        if mask in seen: raise SystemExit("deux qualités identiques : " + x["id"])
        seen.add(mask)
        quals.append((x["id"], mask))
        for a in x.get("aliases", []): aliases.append((a, x["id"]))
    for cid, al in EXTRA.items():
        if cid not in ids: raise SystemExit("EXTRA vers un identifiant inconnu : " + cid)
        for a in al:
            if a not in ids and (a, cid) not in aliases: aliases.append((a, cid))
    names = [a for a, _ in aliases]
    if len(names) != len(set(names)): raise SystemExit("alias en double : " + str([n for n in names if names.count(n) > 1]))
    head = "// Généré par tools/gen_qualities.py depuis le dictionnaire de qualités de Scorsaire. Ne pas modifier à la main.\n"
    out = sys.argv[2]
    open(os.path.join(out, "QualityCatalog.inc"), "w", encoding="utf-8").write(head + "// { identifiant, intervalles (bit n = n demi-tons depuis la fondamentale, jusqu'à 23) }\n" + "\n".join("{ %s, 0x%06X }," % (q(i), m) for i, m in quals) + "\n")
    open(os.path.join(out, "QualityAliases.inc"), "w", encoding="utf-8").write(head + "// { écriture acceptée, identifiant }\n" + "\n".join("{ %s, %s }," % (q(a), q(i)) for a, i in aliases) + "\n")

main()
