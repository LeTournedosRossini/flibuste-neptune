// Carte d'accords partagée : format, double tampon, carte morte, version inconnue, deux processus, coût de lecture.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "neptune/core/SharedChordMap.h"

#ifdef _WIN32
 #include <process.h>
#else
 #include <sys/wait.h>
 #include <unistd.h>
#endif

using namespace neptune::core::chordmap;

namespace
{
std::string uniqueTag()
{
    static std::atomic<int> n { 0 };
#ifdef _WIN32
    return "t" + std::to_string (static_cast<long> (std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)) + "-" + std::to_string (n++) + "-";
#else
    return "t" + std::to_string (static_cast<long> (getpid())) + "-" + std::to_string (n++) + "-";
#endif
}

Container chord (double start, double len, int root, std::uint16_t mask, Kind kind = Kind::Chord)
{
    Container c;
    c.startBeats = start; c.lengthBeats = len; c.kind = kind; c.root = static_cast<std::uint8_t> (root); c.pitchMask = mask;
    return c;
}
constexpr std::uint16_t kMajor = (1u << 0) | (1u << 4) | (1u << 7);
}

TEST_CASE ("chordmap: a reader sees what the writer published", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    r.poll();
    CHECK_FALSE (r.connected());   // pas encore d'écrivain
    REQUIRE (w.open (1) == Status::Ok);
    r.poll();
    CHECK (r.connected());
    CHECK (w.publish ({ chord (0.0, 4.0, 0, kMajor), chord (4.0, 4.0, 7, kMajor << 7 | kMajor >> 5), chord (0.0, 16.0, 2, 0x0AB5, Kind::Key) }) == 3);

    Container c;
    REQUIRE (r.lookup (0.0, Kind::Chord, c));   // début inclus
    CHECK (c.root == 0);
    REQUIRE (r.lookup (3.999, Kind::Chord, c));
    CHECK (c.root == 0);
    REQUIRE (r.lookup (4.0, Kind::Chord, c));   // fin exclue : l'accord suivant
    CHECK (c.root == 7);
    CHECK_FALSE (r.lookup (8.0, Kind::Chord, c));
    CHECK_FALSE (r.lookup (-1.0, Kind::Chord, c));
    REQUIRE (r.lookup (9.0, Kind::Key, c));
    CHECK (c.root == 2);
    REQUIRE (r.next (1.0, Kind::Chord, c));
    CHECK (c.startBeats == 4.0);
    CHECK_FALSE (r.next (4.0, Kind::Chord, c));
    CHECK_FALSE (r.lookup (std::nan (""), Kind::Chord, c));
}

TEST_CASE ("chordmap: publish sorts, drops invalid entries, trims overlaps and caps the size", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (2) == Status::Ok);
    r.setChannel (2);
    r.poll();
    const std::uint16_t m = kMajor;
    auto n = w.publish ({ chord (8.0, 4.0, 5, m), chord (0.0, 6.0, 0, m), chord (4.0, 4.0, 7, m),   // désordre ; le deuxième déborde sur le troisième
                          chord (-1.0, 2.0, 1, m), chord (2.0, 0.0, 1, m), chord (2.0, -3.0, 1, m), chord (1.0, 1.0, 12, m),   // invalides
                          chord (std::nan (""), 1.0, 1, m) });
    CHECK (n == 3);
    Container c;
    REQUIRE (r.lookup (5.0, Kind::Chord, c));
    CHECK (c.root == 7);                                 // le suivant gagne
    REQUIRE (r.lookup (3.9, Kind::Chord, c));
    CHECK (c.root == 0);
    CHECK (c.lengthBeats == 4.0);                        // raccourci à 4

    std::vector<Container> many;
    for (int i = 0; i < kCapacity[0] + 100; ++i) many.push_back (chord (i, 1.0, i % 12, m));
    CHECK (w.publish (many) == kCapacity[0]);
    CHECK_FALSE (r.lookup (kCapacity[0] + 10.0, Kind::Chord, c));
    CHECK (r.lookup (kCapacity[0] - 1.0, Kind::Chord, c));
}

TEST_CASE ("chordmap: republishing replaces the map", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    r.poll();
    w.publish ({ chord (0.0, 4.0, 0, kMajor) });
    const auto s1 = r.sequence();
    w.publish ({ chord (0.0, 4.0, 9, kMajor) });
    CHECK (r.sequence() == s1 + 1);
    Container c;
    REQUIRE (r.lookup (1.0, Kind::Chord, c));
    CHECK (c.root == 9);
    w.publish ({});
    CHECK_FALSE (r.lookup (1.0, Kind::Chord, c));
}

TEST_CASE ("chordmap: a dead map is ignored and a new writer takes over", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    w.publish ({ chord (0.0, 4.0, 0, kMajor) });
    r.poll();
    Container c;
    REQUIRE (r.lookup (1.0, Kind::Chord, c));

    // l'écrivain « disparaît » : plus de signe de vie
    const auto old = neptune::core::chordmap::detail::nowMs() - 2 * kStaleMs;
    w.setHeartbeatForTest (old);
    r.poll();
    CHECK_FALSE (r.connected());
    CHECK_FALSE (r.lookup (1.0, Kind::Chord, c));
    w.touch();   // il revient
    r.poll();
    CHECK (r.lookup (1.0, Kind::Chord, c));
}

TEST_CASE ("chordmap: unknown format version means no map", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    w.publish ({ chord (0.0, 4.0, 0, kMajor) });
    r.poll();
    CHECK (r.connected());
    w.setVersionForTest (kVersion + 1);
    r.poll();
    CHECK_FALSE (r.connected());
    Container c;
    CHECK_FALSE (r.lookup (1.0, Kind::Chord, c));
    w.setVersionForTest (kVersion);
    r.poll();   // il se reconnecte
    CHECK (r.connected());
}

TEST_CASE ("chordmap: the reader loses the map when the writer closes", "[chordmap]")
{
    const auto tag = uniqueTag();
    Reader r (tag);
    {
        Writer w (tag);
        REQUIRE (w.open (1) == Status::Ok);
        w.publish ({ chord (0.0, 4.0, 0, kMajor) });
        r.poll();
        CHECK (r.connected());
    }
    r.poll();
    CHECK_FALSE (r.connected());
}

TEST_CASE ("chordmap: channels are independent", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer a (tag), b (tag);
    REQUIRE (a.open (1) == Status::Ok);
    REQUIRE (b.open (2) == Status::Ok);
    CHECK (a.open (0) == Status::Error);
    CHECK (a.open (9) == Status::Error);
    a.publish ({ chord (0.0, 4.0, 3, kMajor) });
    b.publish ({ chord (0.0, 4.0, 8, kMajor) });
    Reader r1 (tag), r2 (tag);
    r2.setChannel (2);
    r1.poll(); r2.poll();
    Container c;
    REQUIRE (r1.lookup (1.0, Kind::Chord, c));
    CHECK (c.root == 3);
    REQUIRE (r2.lookup (1.0, Kind::Chord, c));
    CHECK (c.root == 8);
}

TEST_CASE ("chordmap: reads stay consistent while the writer republishes", "[chordmap][concurrency]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    w.publish ({ chord (0.0, 100.0, 0, 1u) });
    r.poll();
    std::atomic<bool> stop { false };
    std::atomic<long> reads { 0 }, torn { 0 };
    std::thread reader ([&]
    {
        Container c;
        double beats = 0.0;
        while (! stop.load())
        {
            beats = std::fmod (beats + 0.37, 99.0);
            if (r.lookup (beats, Kind::Chord, c))
            {
                // chaque publication i : tous les conteneurs ont root = i % 12 et le masque 1 << root
                if (c.pitchMask != (1u << c.root)) ++torn;
                ++reads;
            }
        }
    });
    for (int i = 1; i <= 3000; ++i)
    {
        std::vector<Container> v;
        for (int j = 0; j < 50; ++j) v.push_back (chord (j * 2.0, 2.0, i % 12, static_cast<std::uint16_t> (1u << (i % 12))));
        w.publish (v);
    }
    stop = true;
    reader.join();
    CHECK (torn.load() == 0);
    CHECK (reads.load() > 0);
}

#ifndef _WIN32
TEST_CASE ("chordmap: another process reads the map", "[chordmap][process]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    REQUIRE (w.open (3) == Status::Ok);
    w.publish ({ chord (0.0, 4.0, 5, kMajor), chord (4.0, 4.0, 10, kMajor) });
    const pid_t pid = fork();
    REQUIRE (pid >= 0);
    if (pid == 0)
    {
        Reader child (tag);
        child.setChannel (3);
        child.poll();
        Container c;
        const bool ok = child.connected() && child.lookup (5.0, Kind::Chord, c) && c.root == 10 && child.lookup (1.0, Kind::Chord, c) && c.root == 5;
        _exit (ok ? 0 : 1);
    }
    int status = 0;
    REQUIRE (waitpid (pid, &status, 0) == pid);
    CHECK (WIFEXITED (status));
    CHECK (WEXITSTATUS (status) == 0);
}

TEST_CASE ("chordmap: a second live writer in another process is refused, a dead one is replaced", "[chordmap][process]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    REQUIRE (w.open (4) == Status::Ok);
    int status = 0;
    pid_t pid = fork();
    REQUIRE (pid >= 0);
    if (pid == 0) { Writer other (tag); _exit (other.open (4) == Status::InUse ? 0 : 1); }
    REQUIRE (waitpid (pid, &status, 0) == pid);
    CHECK (WEXITSTATUS (status) == 0);

    w.setHeartbeatForTest (neptune::core::chordmap::detail::nowMs() - 2 * kStaleMs);   // le premier écrivain est « mort »
    pid = fork();
    REQUIRE (pid >= 0);
    if (pid == 0) { Writer other (tag); _exit (other.open (4) == Status::Ok ? 0 : 1); }
    REQUIRE (waitpid (pid, &status, 0) == pid);
    CHECK (WEXITSTATUS (status) == 0);
}
#endif

namespace
{
// Lance le programme d'essai (un autre processus) et rend son code de sortie ; -1 si le lancement échoue.
int runProbe (const std::string& mode, const std::string& tag, int channel)
{
    std::string cmd = std::string ("\"") + NEPTUNE_CHORDMAP_PROBE + "\" " + mode + " " + tag + " " + std::to_string (channel);
 #ifdef _WIN32
    cmd = "\"" + cmd + "\"";   // cmd.exe retire une paire de guillemets autour de toute la commande
    return std::system (cmd.c_str());
 #else
    const int status = std::system (cmd.c_str());
    return status == -1 || ! WIFEXITED (status) ? -1 : WEXITSTATUS (status);
 #endif
}
} // namespace

TEST_CASE ("chordmap: a separate program (another process) reads the map and is refused as a second writer", "[chordmap][process][probe]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    REQUIRE (w.open (5) == Status::Ok);
    w.publish ({ chord (0.0, 4.0, 5, kMajor), chord (4.0, 4.0, 10, kMajor) });
    CHECK (runProbe ("read", tag, 5) == 0);
    CHECK (runProbe ("read", tag, 6) == 1);   // autre canal : rien
    CHECK (runProbe ("open", tag, 5) == 2);   // écrivain vivant : refusé
    CHECK (runProbe ("open", tag, 6) == 0);   // canal libre : accepté
}

TEST_CASE ("chordmap: cost of a lookup", "[chordmap][mesure]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    std::vector<Container> v;
    for (int i = 0; i < kCapacity[0]; ++i) v.push_back (chord (i * 2.0, 2.0, i % 12, kMajor));
    w.publish (v);
    r.poll();
    Container c;
    const int N = 5'000'000;
    long hits = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) hits += r.lookup ((i % 1000) * 1.01, Kind::Chord, c) ? 1 : 0;
    const auto ns = std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now() - t0).count() / N;
    std::printf ("\ncoût d'une consultation (16384 accords) : %.1f ns (%ld trouvées sur %d)\n", ns, hits, N);
    CHECK (hits > 0);
}

// ================================================================================================ format v2 : gammes, motifs, mesures

namespace
{
Container measureOf (double start, double len, int num, int den)
{
    Container c;
    c.startBeats = start; c.lengthBeats = len; c.kind = Kind::Measure; c.root = static_cast<std::uint8_t> (num); c.bass = static_cast<std::uint8_t> (den);
    return c;
}
Container patternOf (double start, double len, int slot)
{
    Container c;
    c.startBeats = start; c.lengthBeats = len; c.kind = Kind::Pattern; c.root = static_cast<std::uint8_t> (slot);
    return c;
}
}

TEST_CASE ("chordmap v2: mesures irrégulières (anacrouse, 3/4), numéro de mesure", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    // anacrouse d'un temps, deux mesures de 4/4, puis du 3/4
    w.publish ({ measureOf (0.0, 1.0, 4, 4), measureOf (1.0, 4.0, 4, 4), measureOf (5.0, 4.0, 4, 4), measureOf (9.0, 3.0, 3, 4) });
    r.poll();
    REQUIRE (r.connected());
    CHECK (r.count (Kind::Measure) == 4);
    CHECK (r.indexAt (0.5, Kind::Measure) == 0);
    CHECK (r.indexAt (1.0, Kind::Measure) == 1);
    CHECK (r.indexAt (4.99, Kind::Measure) == 1);
    CHECK (r.indexAt (9.5, Kind::Measure) == 3);
    CHECK (r.indexAt (13.0, Kind::Measure) == -1);   // après la dernière mesure
    Container c;
    REQUIRE (r.lookup (10.0, Kind::Measure, c));
    CHECK (c.root == 3);
    CHECK (c.bass == 4);
    CHECK (c.lengthBeats == 3.0);
    CHECK (r.count (Kind::Chord) == 0);
}

TEST_CASE ("chordmap v2: gammes et motifs sont des voies indépendantes des accords", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    const std::uint16_t kMajorScale = 0xAB5;   // do majeur
    const std::uint16_t kDorian = 0x6AD;       // ré dorien
    // deux accords sous une seule gamme, puis une gamme qui change au milieu d'un accord
    w.publish ({ chord (0.0, 4.0, 0, kMajor), chord (4.0, 4.0, 7, kMajor),
                 chord (0.0, 8.0, 0, kMajorScale, Kind::Scale),
                 chord (8.0, 2.0, 2, kDorian, Kind::Scale),
                 patternOf (0.0, 4.0, 3), patternOf (4.0, 8.0, 7) });
    r.poll();
    Container c;
    REQUIRE (r.lookup (5.0, Kind::Chord, c));
    CHECK (c.root == 7);
    REQUIRE (r.lookup (5.0, Kind::Scale, c));
    CHECK (c.pitchMask == kMajorScale);          // la même gamme sous les deux accords
    REQUIRE (r.lookup (9.0, Kind::Scale, c));
    CHECK (c.root == 2);
    REQUIRE (r.lookup (2.0, Kind::Pattern, c));
    CHECK (c.root == 3);
    REQUIRE (r.lookup (5.0, Kind::Pattern, c));
    CHECK (c.root == 7);
    CHECK_FALSE (r.lookup (20.0, Kind::Pattern, c));
}

TEST_CASE ("chordmap v2: valeurs invalides refusées, une ancienne zone v1 est ignorée", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    CHECK (w.publish ({ patternOf (0.0, 4.0, 0), patternOf (4.0, 4.0, 17), patternOf (8.0, 4.0, 16),
                        measureOf (0.0, 4.0, 0, 4), measureOf (4.0, 4.0, 4, 0), measureOf (8.0, 4.0, 65, 4), measureOf (12.0, 4.0, 4, 4) }) == 2);
    r.poll();
    CHECK (r.count (Kind::Pattern) == 1);
    CHECK (r.count (Kind::Measure) == 1);
    w.setVersionForTest (1);   // un lecteur v2 traite une zone v1 comme « pas de carte »
    r.poll();
    CHECK_FALSE (r.connected());
}

TEST_CASE ("chordmap v2: plafonds de 4096 mesures et 16384 accords", "[chordmap]")
{
    const auto tag = uniqueTag();
    Writer w (tag);
    Reader r (tag);
    REQUIRE (w.open (1) == Status::Ok);
    std::vector<Container> v;
    for (int i = 0; i < 4096 + 50; ++i) v.push_back (measureOf (i * 4.0, 4.0, 4, 4));
    CHECK (w.publish (v) == 4096);
    r.poll();
    CHECK (r.count (Kind::Measure) == 4096);
    CHECK (r.indexAt (4095 * 4.0 + 1.0, Kind::Measure) == 4095);
    static_assert (kCapacity[0] >= 16384, "recommandation de Scorsaire");
    static_assert (kCapacity[4] >= 4096, "recommandation de Scorsaire");
}
