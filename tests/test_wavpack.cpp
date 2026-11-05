#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "neptune/core/SampleLoader.h"
#include "neptune/core/SampleMeta.h"
#include "neptune/core/WavPackFile.h"

#if defined (NEPTUNE_HAS_WAVPACK)
 #include <wavpack.h>
#endif

using namespace neptune::core;
using Catch::Approx;

namespace
{
void put32 (std::vector<std::uint8_t>& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back (static_cast<std::uint8_t> (v >> (8 * i))); }
void putId (std::vector<std::uint8_t>& b, const char* id) { b.insert (b.end(), id, id + 4); }

// En-tête RIFF d'un WAV PCM, arrêté juste après l'identifiant et la taille du chunk `data` (ce que WavPack garde comme enveloppe d'en-tête).
std::vector<std::uint8_t> riffHeader (int channels, int rate, int bits, std::uint32_t dataBytes)
{
    std::vector<std::uint8_t> h;
    putId (h, "RIFF"); put32 (h, 36 + dataBytes); putId (h, "WAVE");
    putId (h, "fmt "); put32 (h, 16);
    h.push_back (1); h.push_back (0);                                      // PCM
    h.push_back (static_cast<std::uint8_t> (channels)); h.push_back (0);
    put32 (h, static_cast<std::uint32_t> (rate));
    put32 (h, static_cast<std::uint32_t> (rate * channels * bits / 8));
    h.push_back (static_cast<std::uint8_t> (channels * bits / 8)); h.push_back (0);
    h.push_back (static_cast<std::uint8_t> (bits)); h.push_back (0);
    putId (h, "data"); put32 (h, dataBytes);
    return h;
}

// Chunks de queue : un `smpl` (note racine, désaccord, boucles) et un `cue ` (positions).
std::vector<std::uint8_t> riffTrailer (int unityNote, std::uint32_t fraction, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& loops, const std::vector<std::uint32_t>& cues)
{
    std::vector<std::uint8_t> t;
    putId (t, "smpl"); put32 (t, 36 + 24 * static_cast<std::uint32_t> (loops.size()));
    put32 (t, 0); put32 (t, 0); put32 (t, 22675); put32 (t, static_cast<std::uint32_t> (unityNote)); put32 (t, fraction); put32 (t, 0); put32 (t, 0);
    put32 (t, static_cast<std::uint32_t> (loops.size())); put32 (t, 0);
    for (std::size_t i = 0; i < loops.size(); ++i) { put32 (t, static_cast<std::uint32_t> (i)); put32 (t, 0); put32 (t, loops[i].first); put32 (t, loops[i].second); put32 (t, 0); put32 (t, 0); }
    putId (t, "cue "); put32 (t, 4 + 24 * static_cast<std::uint32_t> (cues.size()));
    put32 (t, static_cast<std::uint32_t> (cues.size()));
    for (std::size_t i = 0; i < cues.size(); ++i) { put32 (t, static_cast<std::uint32_t> (i + 1)); put32 (t, cues[i]); putId (t, "data"); put32 (t, 0); put32 (t, 0); put32 (t, cues[i]); }
    return t;
}

std::filesystem::path tempFile (const char* name)
{
    return std::filesystem::temp_directory_path() / (std::string ("neptune_wv_test_") + name);
}
} // namespace

TEST_CASE ("wavpack: marks are read from the RIFF wrapper (loops, root note, cue points)")
{
    // 60 + 25 centièmes ; boucle 100..199 (fin incluse) ; deux repères
    const auto m = parseRiffMarks (riffHeader (2, 44100, 16, 4000), riffTrailer (60, 1073741824u, { { 100, 199 } }, { 300, 1500 }));
    CHECK (m.hasRoot);
    CHECK (m.rootNote == 60);
    CHECK (m.detuneCents == Approx (25.0).margin (0.001));
    REQUIRE (m.loops.size() == 1);
    CHECK (m.loops[0].first == 100);
    CHECK (m.loops[0].second == 199);
    CHECK (m.cues == std::vector<std::int64_t> ({ 300, 1500 }));

    // plus de 50 centièmes : la note suivante, désaccordée vers le bas
    const auto up = parseRiffMarks (riffHeader (1, 22050, 16, 10), riffTrailer (59, 3221225472u, {}, {}));
    CHECK (up.rootNote == 60);
    CHECK (up.detuneCents == Approx (-25.0).margin (0.001));

    // sans chunk utile, vide ; en-tête invalide ou tronqué : pas de plantage
    CHECK (parseRiffMarks (riffHeader (2, 44100, 16, 4000), {}).empty());
    CHECK (parseRiffMarks ({}, {}).empty());
    auto cut = riffTrailer (60, 0, { { 1, 2 } }, { 5 });
    cut.resize (cut.size() - 7);
    const auto partial = parseRiffMarks ({}, cut);
    CHECK (partial.loops.size() <= 1);
    std::vector<std::uint8_t> junk (64, 0xFF);
    CHECK (parseRiffMarks (junk, junk).empty());
}

TEST_CASE ("wavpack: a non-WavPack file is not taken for WavPack")
{
    const auto p = tempFile ("plain.wav");
    { std::ofstream f (p, std::ios::binary); f << "RIFF....WAVEfmt "; }
    CHECK_FALSE (isWavPackFile (p));
    CHECK_FALSE (isWavPackFile (tempFile ("does_not_exist.wv")));
    std::filesystem::remove (p);
}

#if defined (NEPTUNE_HAS_WAVPACK)
namespace
{
int writeBlock (void* id, void* data, int32_t count) { return std::fwrite (data, 1, static_cast<std::size_t> (count), static_cast<std::FILE*> (id)) == static_cast<std::size_t> (count); }

// Encode `frames` (entrelacés, entiers de `bits` bits) dans un fichier WavPack sans perte, avec l'enveloppe RIFF donnée.
bool writeWavPack (const std::filesystem::path& path, int channels, int bits, int rate, const std::vector<std::int32_t>& interleaved,
                   const std::vector<std::uint8_t>& header, const std::vector<std::uint8_t>& trailer)
{
    std::FILE* f = std::fopen (path.string().c_str(), "wb");
    if (f == nullptr) return false;
    char error[80] = {};
    WavpackContext* wpc = WavpackOpenFileOutput (writeBlock, f, error);
    if (wpc == nullptr) { std::fclose (f); return false; }
    WavpackConfig cfg {};
    cfg.bytes_per_sample = (bits + 7) / 8;
    cfg.bits_per_sample = bits;
    cfg.num_channels = channels;
    cfg.channel_mask = channels == 1 ? 4 : 3;
    cfg.sample_rate = rate;
    const std::int64_t frames = static_cast<std::int64_t> (interleaved.size()) / channels;
    bool ok = WavpackSetConfiguration64 (wpc, &cfg, frames, nullptr) != 0;
    if (ok && ! header.empty()) ok = WavpackAddWrapper (wpc, const_cast<std::uint8_t*> (header.data()), static_cast<std::uint32_t> (header.size())) != 0;
    ok = ok && WavpackPackInit (wpc) != 0;
    std::vector<std::int32_t> copy = interleaved;
    ok = ok && WavpackPackSamples (wpc, copy.data(), static_cast<std::uint32_t> (frames)) != 0;
    if (ok && ! trailer.empty()) ok = WavpackAddWrapper (wpc, const_cast<std::uint8_t*> (trailer.data()), static_cast<std::uint32_t> (trailer.size())) != 0;
    ok = ok && WavpackFlushSamples (wpc) != 0;
    WavpackCloseFile (wpc);
    std::fclose (f);
    return ok;
}

std::vector<std::int32_t> sineStereo16 (int frames)
{
    std::vector<std::int32_t> v;
    for (int i = 0; i < frames; ++i)
    {
        v.push_back (static_cast<std::int32_t> (std::lround (12000.0 * std::sin (i * 0.05))));
        v.push_back (static_cast<std::int32_t> (std::lround (-9000.0 * std::sin (i * 0.031 + 1.0))));
    }
    return v;
}
} // namespace

TEST_CASE ("wavpack: a file named .wav that holds WavPack decodes exactly, with its loop and cue points")
{
    const int frames = 3000;
    const auto pcm = sineStereo16 (frames);
    const auto path = tempFile ("organ_pipe.wav");   // l'extension ment, comme dans les jeux d'échantillons d'orgue
    REQUIRE (writeWavPack (path, 2, 16, 22050, pcm, riffHeader (2, 22050, 16, frames * 4),
                           riffTrailer (62, 0, { { 500, 1999 } }, { 100, 2400 })));

    CHECK (isWavPackFile (path));
    CHECK (wavPackDecodingAvailable());
    const auto info = probeWavPackFile (path);
    REQUIRE (info.ok);
    CHECK (info.channels == 2);
    CHECK (info.frames == frames);
    CHECK (info.sampleRate == 22050);
    CHECK (info.bitsPerSample == 16);
    CHECK_FALSE (info.isFloat);

    const auto d = decodeWavPackFile (path);
    REQUIRE (d.info.ok);
    REQUIRE (d.samples.size() == pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) REQUIRE (d.samples[i] == Approx (static_cast<float> (pcm[i]) / 32768.0f).margin (1e-7));   // sans perte

    // l'enveloppe du WAV d'origine (44 octets d'en-tête) et ses chunks de queue : ensemble, dans un fichier d'un seul bloc, ou répartis
    CHECK (d.header.size() + d.trailer.size() == 44 + riffTrailer (62, 0, { { 500, 1999 } }, { 100, 2400 }).size());
    const auto marks = readWavPackMarks (path);
    CHECK (marks.rootNote == 62);
    REQUIRE (marks.loops.size() == 1);
    CHECK (marks.loops[0] == std::make_pair<std::int64_t, std::int64_t> (500, 1999));
    CHECK (marks.cues == std::vector<std::int64_t> ({ 100, 2400 }));

    // chargé comme n'importe quel sample : 16 bits gardés en int16, planaire
    const auto probed = probeAudioFile (path);
    REQUIRE (probed.ok);
    CHECK (probed.keptAsInt16);
    CHECK (probed.frames == frames);
    CHECK (probed.sampleRate == Approx (22050.0));
    const auto loaded = loadSampleFile (path);
    REQUIRE (loaded.ok());
    const auto& s = *loaded.sample;
    CHECK (s.isInt16());
    CHECK (s.channels == 2);
    CHECK (s.frames == frames);
    CHECK (s.sampleRate == Approx (22050.0));
    CHECK_FALSE (s.isStreamed());
    for (int i = 0; i < frames; ++i)
    {
        REQUIRE (s.int16Channel (0)[i] == pcm[static_cast<std::size_t> (2 * i)]);
        REQUIRE (s.int16Channel (1)[i] == pcm[static_cast<std::size_t> (2 * i + 1)]);
    }

    // boucles et note racine du fichier, avec la fin de boucle exclusive de Neptune
    SampleMeta meta;
    REQUIRE (readFileMarks (path, meta));
    CHECK (meta.hasRoot);
    CHECK (meta.rootNote == 62);
    REQUIRE (meta.loops.size() == 1);
    CHECK (meta.loops[0].start == 500);
    CHECK (meta.loops[0].end == 2000);
    CHECK (meta.sampleRate == Approx (22050.0));

    // lecture séquentielle (utilisée par le streaming et l'analyse)
    AudioStreamReader reader;
    REQUIRE (reader.open (path));
    CHECK (reader.channels() == 2);
    CHECK (reader.totalFrames() == frames);
    std::vector<float> chunk (200);
    REQUIRE (reader.seek (10));
    CHECK (reader.read (chunk.data(), 100) == 100);
    CHECK (chunk[0] == Approx (static_cast<float> (pcm[20]) / 32768.0f).margin (1e-7));
    CHECK (chunk[1] == Approx (static_cast<float> (pcm[21]) / 32768.0f).margin (1e-7));
    reader.close();   // Windows refuse de supprimer un fichier encore ouvert
    std::filesystem::remove (path);
}

TEST_CASE ("wavpack: 24-bit mono is kept as float, a damaged file is refused")
{
    std::vector<std::int32_t> pcm;
    for (int i = 0; i < 2000; ++i) pcm.push_back (static_cast<std::int32_t> (std::lround (3000000.0 * std::sin (i * 0.02))));
    const auto path = tempFile ("mono24.wv");
    REQUIRE (writeWavPack (path, 1, 24, 48000, pcm, {}, {}));
    const auto loaded = loadSampleFile (path);
    REQUIRE (loaded.ok());
    CHECK_FALSE (loaded.sample->isInt16());
    CHECK (loaded.sample->channels == 1);
    CHECK (loaded.sample->sourceBitDepth == 24);
    for (int i = 0; i < 2000; i += 97) CHECK (loaded.sample->floatChannel (0)[i] == Approx (static_cast<float> (pcm[static_cast<std::size_t> (i)]) / 8388608.0f).margin (1e-7));
    CHECK (readWavPackMarks (path).empty());   // pas d'enveloppe : rien à lire, sans erreur

    // fichier tronqué : refusé proprement
    std::vector<char> bytes;
    { std::ifstream in (path, std::ios::binary); bytes.assign (std::istreambuf_iterator<char> (in), {}); }
    REQUIRE (bytes.size() > 400);
    bytes.resize (bytes.size() / 2);
    const auto cut = tempFile ("mono24_cut.wv");
    { std::ofstream out (cut, std::ios::binary); out.write (bytes.data(), static_cast<std::streamsize> (bytes.size())); }
    const auto bad = loadSampleFile (cut);
    CHECK_FALSE (bad.ok());
    CHECK_FALSE (bad.error.empty());
    std::filesystem::remove (path);
    std::filesystem::remove (cut);
}

TEST_CASE ("wavpack: only the start is loaded when streaming, the rest is read with seeks")
{
    // Assez long pour couvrir plusieurs blocs WavPack ; du bruit déterministe pour que la compression ne réduise pas tout à presque rien.
    constexpr int frames = 300000;
    std::vector<std::int32_t> pcm;
    std::uint32_t seed = 12345u;
    for (int i = 0; i < frames * 2; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        pcm.push_back (static_cast<std::int32_t> ((seed >> 16) % 20000u) - 10000);
    }
    const auto path = tempFile ("long_stereo16.wv");
    REQUIRE (writeWavPack (path, 2, 16, 44100, pcm, {}, {}));

    // début résident seulement : la longueur reste celle du fichier
    const auto part = loadSampleFile (path, 5000);
    REQUIRE (part.ok());
    const auto& s = *part.sample;
    CHECK (s.frames == frames);
    CHECK (s.resident() == 5000);
    CHECK (s.isStreamed());
    CHECK (s.isInt16());
    for (int i = 0; i < 5000; i += 7)
    {
        REQUIRE (s.int16Channel (0)[i] == pcm[static_cast<std::size_t> (2 * i)]);
        REQUIRE (s.int16Channel (1)[i] == pcm[static_cast<std::size_t> (2 * i + 1)]);
    }

    // demande de préchargement plus grande que le fichier : tout est chargé
    const auto whole = loadSampleFile (path, frames + 10);
    REQUIRE (whole.ok());
    CHECK_FALSE (whole.sample->isStreamed());

    // lecture avec positionnement, dans le désordre
    AudioStreamReader reader;
    REQUIRE (reader.open (path));
    CHECK (reader.totalFrames() == frames);
    std::vector<float> chunk (2 * 600);
    for (const int start : { 250000, 5000, 123457, 0, 299000 })
    {
        REQUIRE (reader.seek (start));
        const int want = std::min (600, frames - start);
        REQUIRE (reader.read (chunk.data(), want) == want);
        for (int i = 0; i < want; ++i)
        {
            REQUIRE (chunk[static_cast<std::size_t> (2 * i)] == Approx (static_cast<float> (pcm[static_cast<std::size_t> (2 * (start + i))]) / 32768.0f).margin (1e-7));
            REQUIRE (chunk[static_cast<std::size_t> (2 * i + 1)] == Approx (static_cast<float> (pcm[static_cast<std::size_t> (2 * (start + i) + 1)]) / 32768.0f).margin (1e-7));
        }
    }
    // la fin du fichier : on lit ce qui reste, puis plus rien
    REQUIRE (reader.seek (frames - 100));
    CHECK (reader.read (chunk.data(), 600) == 100);
    CHECK (reader.read (chunk.data(), 600) == 0);
    reader.close();   // Windows refuse de supprimer un fichier encore ouvert
    std::filesystem::remove (path);
}
#endif

// Contrôle sur de vrais fichiers : si NEPTUNE_DEMO_ORGAN_DIR désigne le dossier d'un orgue GrandOrgue décompressé, tous les .wav WavPack qu'il contient
// doivent se charger avec leurs boucles. Sans la variable, ce test ne fait rien.
TEST_CASE ("wavpack: GrandOrgue demo organ samples (optional, NEPTUNE_DEMO_ORGAN_DIR)")
{
    const char* dir = std::getenv ("NEPTUNE_DEMO_ORGAN_DIR");
    if (dir == nullptr || ! wavPackDecodingAvailable()) { SUCCEED ("skipped"); return; }
    int files = 0, withLoop = 0, withCue = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator (dir))
    {
        if (! e.is_regular_file() || ! isWavPackFile (e.path())) continue;
        ++files;
        const auto loaded = loadSampleFile (e.path());
        INFO (e.path().string() << " : " << loaded.error);
        REQUIRE (loaded.ok());
        REQUIRE (loaded.sample->frames > 0);
        const auto marks = readWavPackMarks (e.path());
        withLoop += ! marks.loops.empty();
        withCue += ! marks.cues.empty();
    }
    CHECK (files > 0);
    CHECK (withLoop == files);
    CHECK (withCue == files);
}
