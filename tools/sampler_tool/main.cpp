// neptune_sampler_tool : construction et vérification d'instruments du Sampler de Neptune, sans Reaper.
//   import-sfz  in.sfz out.json [--fade N] [--linear]   convertit un SFZ avec l'import de Neptune (mêmes zones qu'à l'ouverture dans le Sampler) ;
//                                                        --fade N : largeur du fondu entre couches de vélocité, --linear : fondu linéaire (sinon puissance constante)
//   validate    instrument.json                          charge l'instrument comme le plugin, liste les avertissements, la couverture notes x vélocités et les trous
//   sweep       instrument.json --note N [--step S] [--window MS] [--out sweep.wav]
//                                                        joue la note N à toutes les vélocités (1, 1+S, ... 127) avec le vrai moteur et mesure le niveau de l'attaque :
//                                                        un tableau en dB et la liste des marches (variation brusque d'une vélocité à la suivante)
// Le rendu utilise le moteur même du Sampler : ce qu'il mesure est ce qu'on entend dans Neptune et dans Moussaillon.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <sndfile.h>

#include "neptune/core/Instrument.h"
#include "neptune/core/SamplerVoices.h"
#include "neptune/core/SfzImport.h"

using namespace neptune::core;

namespace
{
constexpr double kRate = 48000.0;

std::string readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeWav (const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
    SF_INFO info {};
    info.samplerate = static_cast<int> (kRate);
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* f = sf_open (path.c_str(), SFM_WRITE, &info);
    if (f == nullptr) return false;
    std::vector<float> inter (l.size() * 2);
    for (std::size_t i = 0; i < l.size(); ++i) { inter[2 * i] = l[i]; inter[2 * i + 1] = r[i]; }
    sf_writef_float (f, inter.data(), static_cast<sf_count_t> (l.size()));
    sf_close (f);
    return true;
}

double toDb (double x) { return x > 1e-9 ? 20.0 * std::log10 (x) : -180.0; }

std::string optValue (int argc, char** argv, const char* name, const char* fallback = "")
{
    for (int i = 0; i + 1 < argc; ++i)
        if (std::strcmp (argv[i], name) == 0) return argv[i + 1];
    return fallback;
}
bool hasFlag (int argc, char** argv, const char* name)
{
    for (int i = 0; i < argc; ++i)
        if (std::strcmp (argv[i], name) == 0) return true;
    return false;
}

int cmdImportSfz (int argc, char** argv)
{
    if (argc < 4) { std::fprintf (stderr, "usage: import-sfz in.sfz out.json [--fade N] [--linear]\n"); return 2; }
    const auto result = sfzimport::importSfzFile (argv[2]);
    if (! result.ok) { std::fprintf (stderr, "import impossible : %s\n", result.error.c_str()); return 1; }
    auto desc = result.instrument;
    const int fade = std::atoi (optValue (argc, argv, "--fade", "0").c_str());
    desc.velocityFadeWidth = std::clamp (fade, 0, InstrumentDescription::maxVelocityFadeWidth);
    desc.velocityFadeLinear = hasFlag (argc, argv, "--linear");
    std::ofstream out (argv[3], std::ios::binary);
    out << toJson (desc);
    std::printf ("%d régions, %d zones importées, fondu de vélocité %d (%s)\n", result.regions, result.imported, desc.velocityFadeWidth,
                 desc.velocityFadeLinear ? "linéaire" : "puissance constante");
    for (const auto& w : result.warnings) std::printf ("avertissement : %s\n", w.c_str());
    return 0;
}

int cmdValidate (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: validate instrument.json\n"); return 2; }
    const auto desc = instrumentFromJson (readFile (argv[2]));
    std::vector<std::string> warnings;
    const auto inst = buildInstrument (desc, &warnings);
    std::printf ("zones décrites : %zu ; instrument construit : %s\n", desc.zones.size(), inst != nullptr ? "oui" : "NON");
    for (const auto& w : warnings) std::printf ("avertissement : %s\n", w.c_str());
    // couverture : pour chaque note, les vélocités couvertes par au moins une zone d'attaque
    int holes = 0;
    for (int key = 0; key < 128; ++key)
    {
        std::vector<int> covered (128, 0);
        bool any = false;
        for (const auto& z : desc.zones)
        {
            if (z.onRelease || key < z.keyLow || key > z.keyHigh) continue;
            any = true;
            for (int v = z.velLow; v <= z.velHigh; ++v) covered[static_cast<std::size_t> (v)] = 1;
        }
        if (! any) continue;
        int first = -1;
        for (int v = 1; v <= 127; ++v)
        {
            if (covered[static_cast<std::size_t> (v)] == 0 && first < 0) first = v;
            if ((covered[static_cast<std::size_t> (v)] != 0 || v == 127) && first >= 0)
            {
                const int last = covered[static_cast<std::size_t> (v)] != 0 ? v - 1 : v;
                std::printf ("trou : note %d, vélocités %d à %d sans zone\n", key, first, last);
                ++holes;
                first = -1;
            }
        }
    }
    std::printf ("%d trou(s) de vélocité\n", holes);
    return inst != nullptr ? 0 : 1;
}

struct Level { double peakDb = -180.0, rmsDb = -180.0; };

int cmdSweep (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: sweep instrument.json --note N [--step S] [--window MS] [--out sweep.wav]\n"); return 2; }
    const auto desc = instrumentFromJson (readFile (argv[2]));
    std::vector<std::string> warnings;
    const auto inst = buildInstrument (desc, &warnings);
    if (inst == nullptr) { std::fprintf (stderr, "instrument non construit\n"); return 1; }
    for (const auto& w : warnings) std::printf ("avertissement : %s\n", w.c_str());
    const int note = std::atoi (optValue (argc, argv, "--note", "60").c_str());
    const int step = std::max (1, std::atoi (optValue (argc, argv, "--step", "1").c_str()));
    const double windowMs = std::atof (optValue (argc, argv, "--window", "300").c_str());
    const std::string outPath = optValue (argc, argv, "--out");
    const int holdFrames = static_cast<int> (1.5 * kRate), tailFrames = static_cast<int> (0.4 * kRate);
    const int winFrames = std::clamp (static_cast<int> (windowMs * 0.001 * kRate), 64, holdFrames);

    std::vector<int> vels;
    for (int v = 1; v <= 127; v += step) vels.push_back (v);
    if (vels.back() != 127) vels.push_back (127);

    std::vector<Level> levels;
    std::vector<float> allL, allR;
    for (int v : vels)
    {
        SamplerVoices voices;   // une instance neuve par vélocité : aucune voix ne déborde sur la mesure suivante
        voices.prepare (kRate);
        voices.setInstrument (inst.get());
        voices.noteOn (0, note, static_cast<float> (v) / 127.0f);
        std::vector<float> l (static_cast<std::size_t> (holdFrames + tailFrames), 0.0f), r (l);
        for (int pos = 0; pos < holdFrames; pos += 256)
            voices.render (l.data() + pos, r.data() + pos, std::min (256, holdFrames - pos), 1.0f, 1.0f, 0.0);
        voices.noteOff (0, note);
        for (int pos = holdFrames; pos < holdFrames + tailFrames; pos += 256)
            voices.render (l.data() + pos, r.data() + pos, std::min (256, holdFrames + tailFrames - pos), 1.0f, 1.0f, 0.0);
        Level lv;
        double sum = 0.0, peak = 0.0;
        for (int i = 0; i < winFrames; ++i)
        {
            const double a = l[static_cast<std::size_t> (i)], b = r[static_cast<std::size_t> (i)];
            sum += 0.5 * (a * a + b * b);
            peak = std::max (peak, std::max (std::fabs (a), std::fabs (b)));
        }
        lv.rmsDb = toDb (std::sqrt (sum / winFrames));
        lv.peakDb = toDb (peak);
        levels.push_back (lv);
        allL.insert (allL.end(), l.begin(), l.end());
        allR.insert (allR.end(), r.begin(), r.end());
    }

    // Loi de vélocité attendue : niveau en dB = exposant x 20 log10(vélocité) + constante. On estime l'exposant (médiane des pentes locales, en dB par dB de vélocité) ;
    // une marche est un intervalle dont la variation s'écarte de plus de 1 dB de ce que cette loi prévoit.
    std::vector<double> ratio, expected;
    for (std::size_t i = 1; i < levels.size(); ++i)
    {
        const double lawDb = 20.0 * std::log10 (static_cast<double> (vels[i]) / static_cast<double> (vels[i - 1]));
        expected.push_back (lawDb);
        ratio.push_back ((levels[i].rmsDb - levels[i - 1].rmsDb) / lawDb);
    }
    std::vector<double> sorted = ratio;
    std::sort (sorted.begin(), sorted.end());
    const double exponent = sorted.empty() ? 1.0 : sorted[sorted.size() / 2];
    std::printf ("note %d, fenêtre de mesure %.0f ms (niveau RMS de l'attaque)\n  vél.   RMS dB   crête dB   écart à la loi (dB)\n", note, windowMs);
    int steps = 0;
    std::vector<std::string> notes;
    for (std::size_t i = 0; i < levels.size(); ++i)
    {
        double dev = 0.0;
        if (i > 0) dev = (levels[i].rmsDb - levels[i - 1].rmsDb) - exponent * expected[i - 1];
        std::printf ("  %3d  %7.2f  %8.2f   %s\n", vels[i], levels[i].rmsDb, levels[i].peakDb, i == 0 ? "" : (std::string (dev >= 0 ? "+" : "") + std::to_string (dev).substr (0, 5)).c_str());
        if (i > 0 && std::fabs (dev) > 1.0)
        {
            char buf[160];
            std::snprintf (buf, sizeof buf, "MARCHE : entre la vélocité %d et %d, écart de %+.2f dB par rapport à la loi de vélocité", vels[i - 1], vels[i], dev);
            notes.emplace_back (buf);
            ++steps;
        }
    }
    std::printf ("exposant de vélocité estimé : %.2f (niveau = %.2f x 20 log10 de la vélocité)\n", exponent, exponent);
    for (const auto& n : notes) std::printf ("%s\n", n.c_str());
    std::printf ("%d marche(s) de plus de 1 dB\n", steps);
    if (! outPath.empty() && ! writeWav (outPath, allL, allR)) { std::fprintf (stderr, "impossible d'écrire %s\n", outPath.c_str()); return 1; }
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "commandes : import-sfz, validate, sweep\n"); return 2; }
    const std::string cmd = argv[1];
    if (cmd == "import-sfz") return cmdImportSfz (argc, argv);
    if (cmd == "validate") return cmdValidate (argc, argv);
    if (cmd == "sweep") return cmdSweep (argc, argv);
    std::fprintf (stderr, "commande inconnue : %s\n", cmd.c_str());
    return 2;
}
