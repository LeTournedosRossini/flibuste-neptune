#pragma once
// Carte d'accords partagée entre plugins (prototype) : un écrivain (le futur plugin « Carte ») publie la suite de ses conteneurs
// (accords, tonalités) dans une zone de mémoire partagée du système ; les lecteurs (arpégiateur, instruments...) la consultent avec leur propre
// position de lecture. Voir docs/plan-harmonie-arpegiateur.md.
//   - Zone nommée du système (shm_open sous Linux, file mapping sous Windows) : marche entre fichiers .clap différents ET entre processus.
//   - Double tampon + compteur de génération par tampon : le lecteur (thread audio) ne prend jamais de verrou, n'alloue pas, n'appelle pas le système.
//   - Signe de vie (horloge de l'écrivain) : une carte dont l'écrivain a disparu est ignorée.
//   - Version du format : une version inconnue est traitée comme « pas de carte ».
// Les temps sont en temps de l'hôte (noires, ppq), pas en échantillons.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neptune::core::chordmap
{
constexpr std::uint32_t kMagic = 0x4D48434Eu;   // « NCHM »
constexpr std::uint32_t kVersion = 1;
constexpr int kKinds = 2;
constexpr int kMaxPerKind = 512;
constexpr int kChannels = 8;                    // numéros de liaison 1 à 8
constexpr std::uint64_t kStaleMs = 5000;        // sans signe de vie depuis ce délai : carte morte

enum class Kind : std::uint8_t { Chord = 0, Key = 1 };

struct Container
{
    double startBeats = 0.0;
    double lengthBeats = 0.0;
    Kind kind = Kind::Chord;
    std::uint8_t root = 0;            // 0..11 (do = 0)
    std::uint8_t bass = 255;          // 0..11, 255 : pas de basse distincte
    std::uint8_t reserved = 0;
    std::uint16_t pitchMask = 0;      // classes de hauteur, bit n = note n (absolue, do = 0) : notes de l'accord, ou gamme de la tonalité
    std::uint16_t pad = 0;
};
static_assert (sizeof (Container) == 24, "format partagé : taille figée");

enum class Status { Ok, NotFound, InUse, BadVersion, Error };

namespace detail
{
struct Buffer
{
    std::atomic<std::uint32_t> gen;   // impair pendant l'écriture
    std::uint32_t count[kKinds];
    Container items[kKinds][kMaxPerKind];
};
struct Header
{
    std::uint32_t magic, version, totalSize, writerPid;
    std::atomic<std::uint64_t> heartbeatMs;   // heure (ms depuis 1970) du dernier signe de vie de l'écrivain
    std::atomic<std::uint64_t> sequence;      // incrémenté à chaque publication
    std::atomic<std::uint32_t> active;        // tampon à lire : 0 ou 1
    std::uint32_t reserved;
    Buffer buf[2];
};
static_assert (std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free, "atomiques sans verrou requis");
std::uint64_t nowMs();
} // namespace detail

// Segment mappé (privé à l'implémentation).
struct Region;

class Writer
{
public:
    // `tag` : préfixe de nom (tests) ; vide en usage normal.
    explicit Writer (std::string tag = {});
    ~Writer();
    Writer (const Writer&) = delete;
    Writer& operator= (const Writer&) = delete;

    // Ouvre (crée) la zone du canal 1..8. InUse : un autre écrivain vivant occupe ce canal.
    Status open (int channel);
    void close();   // libère la zone (Linux : la supprime)
    bool isOpen() const noexcept { return header_ != nullptr; }
    // Remplace la carte entière. Trie, retire les invalides, coupe les chevauchements, plafonne. Retourne le nombre de conteneurs publiés.
    int publish (std::vector<Container> containers);
    void touch();   // signe de vie : à appeler environ une fois par seconde depuis le thread de message

    // pour les tests
    void setHeartbeatForTest (std::uint64_t ms);
    void setVersionForTest (std::uint32_t v);

private:
    std::string tag_;
    Region* region_ = nullptr;
    detail::Header* header_ = nullptr;
    int channel_ = 0;
};

class Reader
{
public:
    explicit Reader (std::string tag = {});
    ~Reader();
    Reader (const Reader&) = delete;
    Reader& operator= (const Reader&) = delete;

    // Thread de message (minuterie, environ une fois par seconde) : se connecte si besoin, vérifie version et signe de vie.
    // `nowMs` = 0 : heure du système.
    void poll (std::uint64_t nowMs = 0);
    void setChannel (int channel);
    bool connected() const noexcept { return alive_.load (std::memory_order_relaxed); }
    std::uint64_t sequence() const noexcept;

    // Thread audio : sans verrou, sans allocation, sans appel système. Faux : pas de carte, ou rien à cette position.
    bool lookup (double beats, Kind kind, Container& out) const noexcept;   // conteneur qui couvre `beats` (début inclus, fin exclue)
    bool next (double beats, Kind kind, Container& out) const noexcept;     // premier conteneur qui commence après `beats`

private:
    void disconnect();
    std::string tag_;
    int channel_ = 1;
    Region* region_ = nullptr;
    const detail::Header* header_ = nullptr;
    std::atomic<bool> alive_ { false };
};
} // namespace neptune::core::chordmap
