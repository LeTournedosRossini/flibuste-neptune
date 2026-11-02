#!/usr/bin/env python3
"""Fabrique l'exemple de Façade « Concert » : grille de scènes (clic = rappel, flèches de banque, SET), quatre potards de volume (Master et pistes 1 à 3), muet général,
stepper (Précédent / Suivant), annulation générale. À charger dans la Façade GLOBALE (bouton « Charger ») : les volumes visent les pistes 1 à 4 telles quelles ("track": -1 : aucun rebranchement). Aucun module externe.
Sortie : `concert-scenes/facade.json` et `Concert-scenes.neptunefacade` (zip)."""
import json, os, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "concert-scenes")
os.makedirs(SRC, exist_ok=True)

# identifiants de paramètres (voir core/include/neptune/core/ProbePool.h)
MASTER_VOLUME, MASTER_MUTE = 1, 3
def track(t, offset): return 1000 + 100 * t + offset      # 0 volume, 2 muet
ACTION = lambda command: 1 + command                        # actionMode d'une commande de scène (Combinations.h : 21 annulation, 23 suivant, 24 précédent)

widgets = []
def add(**kw):
    kw["id"] = len(widgets) + 1
    widgets.append(kw)

# la grille de scènes : 8 colonnes x 4 lignes (32 scènes de la banque courante), orange, tempérament vif
add(type=13, x=20, y=20, w=840, h=470, cols=8, rows=4, hue=30, vivid=65, header=1)

# volumes : quatre potards ronds en colonne (Master, puis pistes 1 à 3) ; un glissé vertical les règle ; pas de muets par piste (on baisse le potard à zéro)
labels = [("Master", MASTER_VOLUME)] + [(f"Piste {t + 1}", track(t, 0)) for t in range(3)]
for i, (name, volume) in enumerate(labels):
    add(type=0, x=880, y=20 + i * 115, w=100, h=110, param=volume, text=name)

add(type=1, x=880, y=484, w=100, h=100, param=MASTER_MUTE, text="MUET GÉNÉRAL")

# bas : Set, stepper, annulation générale
add(type=10, x=20, y=510, w=110, h=52, text="Set")
add(type=4, x=140, y=510, w=150, h=52, text="◀ Précédent", mode=ACTION(24))
add(type=4, x=300, y=510, w=150, h=52, text="Suivant ▶", mode=ACTION(23))
add(type=4, x=470, y=510, w=210, h=52, text="Annulation générale", mode=ACTION(21))
add(type=2, x=690, y=510, w=170, h=52, text="Clic droit sur une scène : nom et textes")

doc = {"format": 1, "track": -1, "facade": {"name": "Concert", "fit": 0, "widgets": widgets}}
with open(os.path.join(SRC, "facade.json"), "w", encoding="utf-8") as f:
    json.dump(doc, f, ensure_ascii=False, indent=1)
with zipfile.ZipFile(os.path.join(HERE, "Concert-scenes.neptunefacade"), "w", zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(SRC, "facade.json"), "facade.json")
print(len(widgets), "widgets")
