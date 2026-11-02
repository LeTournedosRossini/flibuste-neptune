#!/usr/bin/env python3
"""Génère une banque de samples SYNTHÉTIQUES pour essayer le constructeur d'instrument : un « piano » de 5 notes x 4 nuances (pp, mp, mf, ff).
Les défauts sont mis exprès : les niveaux d'enregistrement ne suivent pas la nuance de façon régulière (marches à rattraper), et quelques samples sont
désaccordés (à corriger). Les noms suivent le gabarit  <instrument>_<note>_<nuance>.wav  (ex. piano_C3_pp.wav).
Usage : generer_samples_test.py dossier_de_sortie"""
import sys, os
import numpy as np
import soundfile as sf

RATE = 48000
NOTES = {"C2": 36, "G2": 43, "C3": 48, "G3": 55, "C4": 60}
# (nuance, niveau d'enregistrement voulu en dB crête, clarté du timbre) : volontairement irréguliers d'une note à l'autre
DYN = ["pp", "mp", "mf", "ff"]
LEVEL = {"C2": [-34, -24, -12, -4], "G2": [-30, -20, -13, -7], "C3": [-33, -23, -11, -3], "G3": [-28, -22, -14, -6], "C4": [-31, -19, -12, -5]}
DETUNE_CENTS = {("G2", "mp"): 18.0, ("C4", "ff"): -22.0, ("C3", "pp"): 9.0}


def tone(midi, dyn_index, level_db, detune_cents, seconds=3.0):
    f0 = 440.0 * 2 ** ((midi - 69) / 12.0) * 2 ** (detune_cents / 1200.0)
    t = np.arange(int(seconds * RATE)) / RATE
    brightness = 0.35 + 0.2 * dyn_index   # plus fort = plus brillant, comme un vrai piano
    sig = np.zeros_like(t)
    for h in range(1, 14):
        amp = (brightness ** (h - 1)) / h
        decay = np.exp(-t * (1.2 + 0.5 * h) / (1.0 + 0.2 * (midi - 36) / 12.0))
        sig += amp * decay * np.sin(2 * np.pi * f0 * h * t + 0.3 * h)
    attack = np.minimum(1.0, t / 0.004)
    sig *= attack
    rng = np.random.default_rng(midi * 10 + dyn_index)
    sig += 0.0008 * rng.standard_normal(len(t)) * np.exp(-t * 3)   # un souffle très léger
    sig *= 10 ** (level_db / 20.0) / np.max(np.abs(sig))
    return sig.astype(np.float32)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, midi in NOTES.items():
        for i, dyn in enumerate(DYN):
            cents = DETUNE_CENTS.get((name, dyn), 0.0)
            sig = tone(midi, i, LEVEL[name][i], cents)
            sf.write(os.path.join(out, f"piano_{name}_{dyn}.wav"), sig, RATE, subtype="FLOAT")
    print(f"{len(NOTES) * len(DYN)} samples écrits dans {out}")
    print("désaccords mis exprès :", {f'{k[0]}_{k[1]}': v for k, v in DETUNE_CENTS.items()})


if __name__ == "__main__":
    main()
