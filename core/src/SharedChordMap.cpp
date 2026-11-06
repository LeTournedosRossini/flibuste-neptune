#include "neptune/core/SharedChordMap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <sys/mman.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace neptune::core::chordmap
{
namespace detail
{
std::uint64_t nowMs()
{
    return static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now().time_since_epoch()).count());
}
} // namespace detail

struct Region
{
    void* base = nullptr;
    std::size_t size = 0;
    std::string name;
    bool created = false;
#ifdef _WIN32
    void* handle = nullptr;
#else
    int fd = -1;
#endif
};

namespace
{
std::string regionName (const std::string& tag, int channel)
{
#ifdef _WIN32
    return "Local\\neptune-chords-" + tag + std::to_string (channel);
#elif defined(__APPLE__)
    // macOS limite le nom d'une zone partagée à 31 caractères : on garde une empreinte du nom complet (FNV-1a), la même pour l'écrivain et les lecteurs.
    std::uint64_t h = 1469598103934665603ull;
    for (char c : tag + "|" + std::to_string (channel))
        h = (h ^ static_cast<unsigned char> (c)) * 1099511628211ull;
    char buf[32];
    std::snprintf (buf, sizeof buf, "/nc-%lx-%012llx", static_cast<long> (getuid()), static_cast<unsigned long long> (h & 0xFFFFFFFFFFFFull));
    return buf;
#else
    return "/neptune-chords-" + std::to_string (static_cast<long> (getuid())) + "-" + tag + std::to_string (channel);
#endif
}

std::uint32_t processId()
{
#ifdef _WIN32
    return static_cast<std::uint32_t> (GetCurrentProcessId());
#else
    return static_cast<std::uint32_t> (getpid());
#endif
}

void release (Region* r, bool unlinkName)
{
    if (r == nullptr) return;
#ifdef _WIN32
    if (r->base != nullptr) UnmapViewOfFile (r->base);
    if (r->handle != nullptr) CloseHandle (r->handle);
    (void) unlinkName;
#else
    if (r->base != nullptr) munmap (r->base, r->size);
    if (r->fd >= 0) close (r->fd);
    if (unlinkName) shm_unlink (r->name.c_str());
#endif
    delete r;
}

// Crée ou ouvre la zone en lecture-écriture (écrivain).
Region* mapWritable (const std::string& name, std::size_t size)
{
    auto* r = new Region;
    r->name = name;
    r->size = size;
#ifdef _WIN32
    r->handle = CreateFileMappingA (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD> (static_cast<std::uint64_t> (size) >> 32), static_cast<DWORD> (size & 0xFFFFFFFFu), name.c_str());
    if (r->handle == nullptr) { release (r, false); return nullptr; }
    r->created = GetLastError() != ERROR_ALREADY_EXISTS;
    r->base = MapViewOfFile (r->handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
    r->fd = shm_open (name.c_str(), O_CREAT | O_RDWR, 0600);
    if (r->fd < 0) { release (r, false); return nullptr; }
    struct stat st {};
    if (fstat (r->fd, &st) != 0) { release (r, false); return nullptr; }
    r->created = st.st_size == 0;
    if (static_cast<std::size_t> (st.st_size) < size && ftruncate (r->fd, static_cast<off_t> (size)) != 0) { release (r, false); return nullptr; }
    void* p = mmap (nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    r->base = p == MAP_FAILED ? nullptr : p;
#endif
    if (r->base == nullptr) { release (r, false); return nullptr; }
    return r;
}

// Ouvre une zone existante en lecture seule (lecteur) ; nullptr si absente.
Region* mapReadOnly (const std::string& name, std::size_t size)
{
    auto* r = new Region;
    r->name = name;
    r->size = size;
#ifdef _WIN32
    r->handle = OpenFileMappingA (FILE_MAP_READ, FALSE, name.c_str());
    if (r->handle == nullptr) { release (r, false); return nullptr; }
    r->base = MapViewOfFile (r->handle, FILE_MAP_READ, 0, 0, size);
#else
    r->fd = shm_open (name.c_str(), O_RDONLY, 0);
    if (r->fd < 0) { release (r, false); return nullptr; }
    struct stat st {};
    if (fstat (r->fd, &st) != 0 || static_cast<std::size_t> (st.st_size) < size) { release (r, false); return nullptr; }   // zone trop petite : format inconnu
    void* p = mmap (nullptr, size, PROT_READ, MAP_SHARED, r->fd, 0);
    r->base = p == MAP_FAILED ? nullptr : p;
#endif
    if (r->base == nullptr) { release (r, false); return nullptr; }
    return r;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- écrivain

Writer::Writer (std::string tag) : tag_ (std::move (tag)) {}
Writer::~Writer() { close(); }

Status Writer::open (int channel)
{
    if (channel < 1 || channel > kChannels) return Status::Error;
    close();
    auto* r = mapWritable (regionName (tag_, channel), sizeof (detail::Header));
    if (r == nullptr) return Status::Error;
    auto* h = static_cast<detail::Header*> (r->base);
    const auto now = detail::nowMs();
    // Une zone déjà là, d'un format reconnu, avec un écrivain vivant d'un autre processus : on ne la prend pas.
    if (h->magic == kMagic && h->version == kVersion && h->writerPid != processId() && now - std::min (now, h->heartbeatMs.load()) < kStaleMs)
    {
        release (r, false);
        return Status::InUse;
    }
    // (Ré)initialisation : le magic est écrit en dernier, un lecteur ne prend la zone qu'une fois prête.
    h->magic = 0;
    std::memset (static_cast<void*> (h->buf), 0, sizeof (h->buf));
    h->active.store (0);
    h->sequence.store (0);
    h->totalSize = static_cast<std::uint32_t> (sizeof (detail::Header));
    h->writerPid = processId();
    h->heartbeatMs.store (now);
    h->version = kVersion;
    std::atomic_thread_fence (std::memory_order_release);
    h->magic = kMagic;
    region_ = r;
    header_ = h;
    channel_ = channel;
    return Status::Ok;
}

void Writer::close()
{
    if (region_ == nullptr) return;
    if (header_ != nullptr) header_->magic = 0;   // les lecteurs voient la carte disparaître
    release (region_, true);
    region_ = nullptr;
    header_ = nullptr;
}

int Writer::publish (std::vector<Container> containers)
{
    if (header_ == nullptr) return 0;
    std::vector<Container> byKind[kKinds];
    for (auto c : containers)
    {
        const int k = static_cast<int> (c.kind);
        if (k < 0 || k >= kKinds) continue;
        if (! std::isfinite (c.startBeats) || ! std::isfinite (c.lengthBeats) || c.lengthBeats <= 0.0 || c.startBeats < 0.0) continue;
        switch (c.kind)
        {
            case Kind::Pattern:
                if (c.root < 1 || c.root > 16) continue;
                c.bass = 255;
                c.pitchMask = 0;
                break;
            case Kind::Measure:
                if (c.root < 1 || c.root > 64 || c.bass < 1 || c.bass > 64) continue;
                c.pitchMask = 0;
                break;
            default:
                if (c.root > 11) continue;
                if (c.bass > 11 && c.bass != 255) c.bass = 255;
                c.pitchMask = static_cast<std::uint16_t> (c.pitchMask & 0x0FFFu);
                break;
        }
        c.reserved = 0;
        c.pad = 0;
        byKind[k].push_back (c);
    }
    int total = 0;
    for (auto& v : byKind)
    {
        std::stable_sort (v.begin(), v.end(), [] (const Container& a, const Container& b) { return a.startBeats < b.startBeats; });
        // chevauchement : le suivant gagne, le précédent est raccourci (ou retiré s'il disparaît)
        std::vector<Container> clean;
        for (const auto& c : v)
        {
            if (! clean.empty() && clean.back().startBeats + clean.back().lengthBeats > c.startBeats)
            {
                clean.back().lengthBeats = c.startBeats - clean.back().startBeats;
                if (clean.back().lengthBeats <= 0.0) clean.pop_back();
            }
            clean.push_back (c);
        }
        const int kind = static_cast<int> (&v - byKind);
        if (clean.size() > static_cast<std::size_t> (kCapacity[kind])) clean.resize (static_cast<std::size_t> (kCapacity[kind]));
        v = std::move (clean);
        total += static_cast<int> (v.size());
    }
    auto* h = header_;
    const std::uint32_t target = 1u - (h->active.load (std::memory_order_relaxed) & 1u);
    auto& b = h->buf[target];
    b.gen.fetch_add (1, std::memory_order_acq_rel);   // impair : écriture en cours
    for (int k = 0; k < kKinds; ++k)
    {
        b.count[k] = static_cast<std::uint32_t> (byKind[k].size());
        if (! byKind[k].empty()) std::memcpy (static_cast<void*> (b.items + offsetOf (k)), byKind[k].data(), byKind[k].size() * sizeof (Container));
    }
    b.gen.fetch_add (1, std::memory_order_release);   // pair : terminé
    h->active.store (target, std::memory_order_release);
    h->sequence.fetch_add (1, std::memory_order_acq_rel);
    h->heartbeatMs.store (detail::nowMs(), std::memory_order_relaxed);
    return total;
}

void Writer::touch()
{
    if (header_ != nullptr) header_->heartbeatMs.store (detail::nowMs(), std::memory_order_relaxed);
}

void Writer::setHeartbeatForTest (std::uint64_t ms) { if (header_ != nullptr) header_->heartbeatMs.store (ms); }
void Writer::setVersionForTest (std::uint32_t v) { if (header_ != nullptr) header_->version = v; }

// ---------------------------------------------------------------------------------------------------- lecteur

Reader::Reader (std::string tag) : tag_ (std::move (tag)) {}
Reader::~Reader() { disconnect(); }

void Reader::setChannel (int channel)
{
    if (channel < 1 || channel > kChannels || channel == channel_) return;
    disconnect();
    channel_ = channel;
}

void Reader::disconnect()
{
    alive_.store (false);
    header_ = nullptr;
    release (region_, false);
    region_ = nullptr;
}

void Reader::poll (std::uint64_t now)
{
    if (now == 0) now = detail::nowMs();
    if (region_ == nullptr)
    {
        region_ = mapReadOnly (regionName (tag_, channel_), sizeof (detail::Header));
        if (region_ == nullptr) { alive_.store (false); return; }
        header_ = static_cast<const detail::Header*> (region_->base);
    }
    const auto* h = header_;
    const bool ok = h->magic == kMagic && h->version == kVersion && h->totalSize == sizeof (detail::Header)
                 && now - std::min (now, h->heartbeatMs.load (std::memory_order_relaxed)) < kStaleMs;
    if (! ok && (h->magic != kMagic || h->version != kVersion || h->totalSize != sizeof (detail::Header)))
    {
        // zone supprimée, réinitialisée ou d'un autre format : on lâche la zone, on réessaiera au prochain poll
        disconnect();
        return;
    }
    alive_.store (ok);
}

std::uint64_t Reader::sequence() const noexcept
{
    return header_ != nullptr ? header_->sequence.load (std::memory_order_relaxed) : 0;
}

namespace
{
enum class Mode { Cover, After };

bool query (const detail::Header* h, double beats, Kind kind, Container& out, Mode mode, int* index = nullptr) noexcept
{
    const int k = static_cast<int> (kind);
    if (h == nullptr || k < 0 || k >= kKinds || std::isnan (beats)) return false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const detail::Buffer& b = h->buf[h->active.load (std::memory_order_acquire) & 1u];
        const std::uint32_t g1 = b.gen.load (std::memory_order_acquire);
        if ((g1 & 1u) != 0) continue;
        const Container* items = b.items + offsetOf (k);
        const std::uint32_t n = std::min<std::uint32_t> (b.count[k], static_cast<std::uint32_t> (kCapacity[k]));
        // premier conteneur dont le début est > beats ; Cover prend celui d'avant (s'il couvre beats), After prend celui-là
        std::uint32_t lo = 0, hi = n;
        while (lo < hi)
        {
            const std::uint32_t mid = (lo + hi) / 2;
            if (items[mid].startBeats <= beats) lo = mid + 1; else hi = mid;
        }
        bool found = false;
        Container c;
        if (mode == Mode::Cover)
        {
            if (lo > 0) { c = items[lo - 1]; found = beats < c.startBeats + c.lengthBeats; }
        }
        else if (lo < n) { c = items[lo]; found = true; }
        std::atomic_thread_fence (std::memory_order_acquire);
        if (b.gen.load (std::memory_order_relaxed) != g1) continue;   // réécrit pendant la lecture : on recommence
        if (found)
        {
            out = c;
            if (index != nullptr) *index = mode == Mode::Cover ? static_cast<int> (lo) - 1 : static_cast<int> (lo);
        }
        return found;
    }
    return false;
}
} // namespace

bool Reader::lookup (double beats, Kind kind, Container& out) const noexcept
{
    return alive_.load (std::memory_order_relaxed) && query (header_, beats, kind, out, Mode::Cover);
}

bool Reader::next (double beats, Kind kind, Container& out) const noexcept
{
    return alive_.load (std::memory_order_relaxed) && query (header_, beats, kind, out, Mode::After);
}

int Reader::indexAt (double beats, Kind kind) const noexcept
{
    Container c;
    int idx = -1;
    if (! alive_.load (std::memory_order_relaxed) || ! query (header_, beats, kind, c, Mode::Cover, &idx)) return -1;
    return idx;
}

int Reader::count (Kind kind) const noexcept
{
    const int k = static_cast<int> (kind);
    if (! alive_.load (std::memory_order_relaxed) || header_ == nullptr || k < 0 || k >= kKinds) return 0;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const detail::Buffer& b = header_->buf[header_->active.load (std::memory_order_acquire) & 1u];
        const std::uint32_t g1 = b.gen.load (std::memory_order_acquire);
        if ((g1 & 1u) != 0) continue;
        const int n = static_cast<int> (std::min<std::uint32_t> (b.count[k], static_cast<std::uint32_t> (kCapacity[k])));
        std::atomic_thread_fence (std::memory_order_acquire);
        if (b.gen.load (std::memory_order_relaxed) == g1) return n;
    }
    return 0;
}
} // namespace neptune::core::chordmap
