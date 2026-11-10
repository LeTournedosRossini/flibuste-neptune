# Convolution : moteur et bibliothèques tierces

- `core/include/neptune/core/conv/*.hpp` : moteur de convolution à latence nulle de **LowLatConv** (https://github.com/LeTournedosRossini/LowLatConv), repris tel quel
  (seuls les chemins d'inclusion ont changé), publié ici sous la même licence que Neptune (AGPLv3).
- `third_party/pffft/` : **PFFFT** (FFT à instructions SIMD), licence de type BSD, voir `third_party/pffft/LICENSE.txt` (copie du fichier amont).
- `third_party/dr_wav/` : **dr_wav** (lecture de WAV, domaine public ou MIT-0, mackron/dr_libs), un en-tête et sa compilation unique `dr_wav_impl.cpp`.
- Les tests d'origine du moteur sont dans `tests/conv/` (leur `main` est renommé, voir `tests/CMakeLists.txt`).
