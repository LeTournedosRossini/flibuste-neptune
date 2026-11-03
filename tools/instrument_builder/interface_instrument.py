#!/usr/bin/env python3
"""Interface visuelle du constructeur d'instrument (Tkinter, fournie avec Python).

    python3 interface_instrument.py [dossier_de_samples]

Choisir le dossier (celui des samples découpés, par exemple <session>/samples), dire comment les dynamiques sont réparties, puis « Construire »
(mesure, gains, justesse, instrument .json) et « Vérifier » (balayage de vélocité avec le vrai moteur du Sampler sur chaque note et entre les notes).
Tout passe par les mêmes outils que la ligne de commande : construire_instrument.py et neptune_sampler_tool."""
import os
import re
import subprocess
import sys
import threading
import queue
import tkinter as tk
from tkinter import filedialog, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import construire_instrument as ci  # noqa: E402


class App(tk.Tk):
    def __init__(self, folder=""):
        super().__init__()
        self.title("Constructeur d'instrument Neptune")
        self.geometry("980x760")
        self.minsize(820, 620)
        self.out = queue.Queue()
        self.busy = False
        self.tool = None
        try:
            self.tool = ci.find_tool(None)
        except SystemExit:
            self.tool = None

        self.folder = tk.StringVar(value=folder)
        self.dyn_mode = tk.StringVar(value="noms")
        self.first, self.last, self.count = tk.StringVar(value="5"), tk.StringVar(value="127"), tk.StringVar(value="24")
        self.discrete = tk.StringVar(value="10,20,30,40,60")
        self.measure = tk.StringVar(value="lufs")
        self.window, self.search = tk.StringVar(value="400"), tk.StringVar(value="3")
        self.raw, self.notune = tk.BooleanVar(value=False), tk.BooleanVar(value=False)
        self.fade, self.linear = tk.StringVar(value="0"), tk.BooleanVar(value=False)
        self.status = tk.StringVar(value="")
        self.build_ui()
        self.after(100, self.pump)
        if not self.tool:
            self.log("neptune_sampler_tool introuvable : le construire (cible CMake neptune_sampler_tool). Le bouton « Construire » en a besoin.\n")

    # ------------------------------------------------------------------ interface
    def build_ui(self):
        pad = dict(padx=10, pady=6)
        top = ttk.LabelFrame(self, text="1. Dossier des samples")
        top.pack(fill="x", **pad)
        ttk.Entry(top, textvariable=self.folder).pack(side="left", fill="x", expand=True, padx=8, pady=8)
        ttk.Button(top, text="Parcourir...", command=self.browse).pack(side="left", padx=8)

        dyn = ttk.LabelFrame(self, text="2. Dynamiques (quelles vélocités ont servi à enregistrer)")
        dyn.pack(fill="x", **pad)
        r1 = ttk.Frame(dyn)
        r1.pack(fill="x", padx=8, pady=3)
        ttk.Radiobutton(r1, text="D'après les noms des fichiers (v40, p7, pp...)", variable=self.dyn_mode, value="noms").pack(side="left")
        r2 = ttk.Frame(dyn)
        r2.pack(fill="x", padx=8, pady=3)
        ttk.Radiobutton(r2, text="Plage : de", variable=self.dyn_mode, value="plage").pack(side="left")
        for var, label in ((self.first, "à"), (self.last, "en")):
            ttk.Entry(r2, textvariable=var, width=5).pack(side="left", padx=3)
            ttk.Label(r2, text=label).pack(side="left")
        ttk.Entry(r2, textvariable=self.count, width=5).pack(side="left", padx=3)
        ttk.Label(r2, text="paliers (noms avec p<palier> seulement, ou v<vélocité>)").pack(side="left")
        r3 = ttk.Frame(dyn)
        r3.pack(fill="x", padx=8, pady=3)
        ttk.Radiobutton(r3, text="Niveaux discrets :", variable=self.dyn_mode, value="discrets").pack(side="left")
        ttk.Entry(r3, textvariable=self.discrete, width=34).pack(side="left", padx=6)
        ttk.Button(r3, text="Voir la carte", command=self.show_plan).pack(side="right")

        opt = ttk.LabelFrame(self, text="3. Mesure et corrections")
        opt.pack(fill="x", **pad)
        o1 = ttk.Frame(opt)
        o1.pack(fill="x", padx=8, pady=3)
        ttk.Label(o1, text="Mesure :").pack(side="left")
        ttk.Combobox(o1, textvariable=self.measure, values=["lufs", "rms", "crete"], width=7, state="readonly").pack(side="left", padx=4)
        ttk.Label(o1, text="fenêtre (ms)").pack(side="left", padx=(12, 2))
        ttk.Entry(o1, textvariable=self.window, width=6).pack(side="left")
        ttk.Label(o1, text="recherche (s)").pack(side="left", padx=(12, 2))
        ttk.Entry(o1, textvariable=self.search, width=5).pack(side="left")
        ttk.Label(o1, text="largeur du fondu entre couches").pack(side="left", padx=(12, 2))
        ttk.Entry(o1, textvariable=self.fade, width=4).pack(side="left")
        ttk.Checkbutton(o1, text="linéaire", variable=self.linear).pack(side="left", padx=6)
        o2 = ttk.Frame(opt)
        o2.pack(fill="x", padx=8, pady=3)
        ttk.Checkbutton(o2, text="Sans correction de gain ni d'accord (pour comparer avant / après)", variable=self.raw).pack(side="left")
        ttk.Checkbutton(o2, text="Ne pas corriger la justesse", variable=self.notune).pack(side="left", padx=14)

        act = ttk.Frame(self)
        act.pack(fill="x", **pad)
        self.btn_build = ttk.Button(act, text="Construire l'instrument", command=self.build)
        self.btn_build.pack(side="left")
        self.btn_check = ttk.Button(act, text="Vérifier (balayage de vélocité)", command=self.check)
        self.btn_check.pack(side="left", padx=8)
        ttk.Button(act, text="Ouvrir le dossier", command=self.open_folder).pack(side="left")
        ttk.Button(act, text="Effacer le journal", command=lambda: self.text.delete("1.0", "end")).pack(side="right")

        self.statusbar = ttk.Label(self, textvariable=self.status, font=("TkDefaultFont", 11, "bold"))
        self.statusbar.pack(fill="x", padx=12)
        frame = ttk.Frame(self)
        frame.pack(fill="both", expand=True, padx=10, pady=(4, 10))
        self.text = tk.Text(frame, wrap="none", font=("DejaVu Sans Mono", 9))
        sy = ttk.Scrollbar(frame, command=self.text.yview)
        sx = ttk.Scrollbar(frame, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=sy.set, xscrollcommand=sx.set)
        sy.pack(side="right", fill="y")
        sx.pack(side="bottom", fill="x")
        self.text.pack(fill="both", expand=True)
        self.text.tag_configure("bad", foreground="#b00020")
        self.text.tag_configure("good", foreground="#1b7a2e")

    # ------------------------------------------------------------------ utilitaires
    def browse(self):
        d = filedialog.askdirectory(title="Dossier des samples", initialdir=self.folder.get() or os.path.expanduser("~"))
        if d:
            self.folder.set(d)

    def open_folder(self):
        if self.folder.get():
            subprocess.Popen(["xdg-open" if sys.platform.startswith("linux") else "open", self.folder.get()])

    def log(self, s):
        tag = ""
        if "MARCHE" in s or "IGNORÉ" in s or "peu sûr" in s or "non mesurable" in s or "introuvable" in s or "NON" in s:
            tag = "bad"
        self.text.insert("end", s, tag)
        self.text.see("end")

    def pump(self):
        try:
            while True:
                kind, payload = self.out.get_nowait()
                if kind == "log":
                    self.log(payload)
                elif kind == "status":
                    self.status.set(payload[0])
                    self.statusbar.configure(foreground=payload[1])
                elif kind == "done":
                    self.busy = False
                    self.btn_build.state(["!disabled"])
                    self.btn_check.state(["!disabled"])
        except queue.Empty:
            pass
        self.after(100, self.pump)

    def dyn_args(self):
        mode = self.dyn_mode.get()
        if mode == "plage":
            return ["--plage", f"{self.first.get()}:{self.last.get()}:{self.count.get()}"]
        if mode == "discrets":
            return ["--velocites", self.discrete.get().replace(" ", "")]
        return []

    def run_cmd(self, argv):
        self.out.put(("log", "$ " + " ".join(argv) + "\n"))
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        lines = []
        for line in p.stdout:
            lines.append(line)
            self.out.put(("log", line))
        p.wait()
        return p.returncode, "".join(lines)

    def start(self, target):
        if self.busy:
            return
        if not self.folder.get() or not os.path.isdir(self.folder.get()):
            self.status.set("Choisir d'abord un dossier de samples.")
            self.statusbar.configure(foreground="#b00020")
            return
        self.busy = True
        self.btn_build.state(["disabled"])
        self.btn_check.state(["disabled"])
        threading.Thread(target=self.guard, args=(target,), daemon=True).start()

    def guard(self, target):
        try:
            target()
        except Exception as e:  # noqa: BLE001 - l'interface doit survivre à une erreur d'outil
            self.out.put(("log", f"Erreur : {e}\n"))
            self.out.put(("status", ("Erreur (voir le journal)", "#b00020")))
        finally:
            self.out.put(("done", None))

    # ------------------------------------------------------------------ actions
    def show_plan(self):
        if self.dyn_mode.get() == "noms":
            self.log("Choisir « Plage » ou « Niveaux discrets » pour voir la carte.\n")
            return
        argv = [sys.executable, os.path.join(HERE, "construire_instrument.py"), "--plan"] + self.dyn_args()
        threading.Thread(target=lambda: self.run_cmd(argv), daemon=True).start()

    def newest_sfz(self):
        sfz = [os.path.join(self.folder.get(), f) for f in os.listdir(self.folder.get()) if f.endswith(".sfz")]
        return max(sfz, key=os.path.getmtime) if sfz else None

    def build(self):
        self.start(self._build)

    def _build(self):
        if not self.tool:
            self.out.put(("status", ("neptune_sampler_tool introuvable", "#b00020")))
            return
        d = self.folder.get()
        argv = [sys.executable, os.path.join(HERE, "construire_instrument.py"), d, "--tool", self.tool, "--mesure", self.measure.get(),
                "--window", self.window.get(), "--search", self.search.get()] + self.dyn_args()
        if self.raw.get():
            argv.append("--raw")
        if self.notune.get():
            argv.append("--no-tune")
        self.out.put(("status", ("Mesure en cours...", "#555555")))
        code, _ = self.run_cmd(argv)
        if code != 0:
            self.out.put(("status", ("Construction impossible (voir le journal)", "#b00020")))
            return
        sfz = self.newest_sfz()
        js = sfz[:-4] + ".json"
        imp = [self.tool, "import-sfz", sfz, js, "--fade", self.fade.get()] + (["--linear"] if self.linear.get() else [])
        code, _ = self.run_cmd(imp)
        if code != 0:
            self.out.put(("status", ("Import impossible (voir le journal)", "#b00020")))
            return
        code, text = self.run_cmd([self.tool, "validate", js])
        holes = re.search(r"(\d+) trou", text)
        n = int(holes.group(1)) if holes else 0
        if code != 0 or n > 0:
            self.out.put(("status", (f"Instrument écrit, mais {n} trou(s) de vélocité : voir le journal", "#b00020")))
        else:
            self.out.put(("status", (f"Instrument prêt : {js}  — lancer « Vérifier » pour contrôler les marches", "#1b7a2e")))

    def check(self):
        self.start(self._check)

    def _check(self):
        sfz = self.newest_sfz()
        if not sfz or not self.tool:
            self.out.put(("status", ("Construire d'abord l'instrument", "#b00020")))
            return
        js = sfz[:-4] + ".json"
        roots = sorted({int(m) for m in re.findall(r"pitch_keycenter=(\d+)", open(sfz).read())})
        notes = list(roots)
        for a, b in zip(roots, roots[1:]):
            if b - a > 1:
                notes.append((a + b) // 2)   # une note non échantillonnée entre deux samples : elle est transposée
        notes = sorted(set(notes))
        total = 0
        for i, n in enumerate(notes):
            self.out.put(("status", (f"Balayage de la note {n} ({i + 1}/{len(notes)})...", "#555555")))
            _, text = self.run_cmd([self.tool, "sweep", js, "--note", str(n), "--step", "2", "--mesure", self.measure.get(),
                                    "--window", self.window.get(), "--search", self.search.get()])
            m = re.search(r"(\d+) marche", text)
            total += int(m.group(1)) if m else 0
        if total:
            self.out.put(("status", (f"{total} marche(s) de plus de 1 dB sur {len(notes)} notes : voir les lignes MARCHE du journal", "#b00020")))
        else:
            self.out.put(("status", (f"Aucune marche de plus de 1 dB sur {len(notes)} notes (dont {len(notes) - len(roots)} non échantillonnées)", "#1b7a2e")))


if __name__ == "__main__":
    App(sys.argv[1] if len(sys.argv) > 1 else "").mainloop()
