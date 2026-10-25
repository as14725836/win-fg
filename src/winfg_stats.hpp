// win-fg — frame-generation statistics exposed to overlays (MangoHud, etc.)
//
// win-fg runs as a Vulkan implicit layer. An overlay such as MangoHudfex needs to
// show the *post*-interpolation frame rate (real + generated), but it may sit on
// either side of win-fg in the layer chain, so it cannot simply count presents
// itself. Instead win-fg publishes per-frame counters into a small shared
// segment, which the overlay reads every frame.
//
// Segment name: "win-fg-<pid>-stats".
//
// BACKENDS (tried in this order, first one that works wins):
//   1. $WIN_FG_STATS_DIR (or legacy $WIN_FG_SHM_DIR) -> plain file
//      "<dir>/<name>". Works on every libc including Bionic, and is the only
//      backend that lets a DIFFERENT process attach without an fd hand-off.
//   2. __ANDROID__ && __ANDROID_API__ >= 26 -> ASharedMemory_create().
//      Bionic has no shm_open(). v0.3.1 shipped with the whole publisher
//      compiled out under a bare #ifdef __ANDROID__, which killed telemetry on
//      exactly the platform win-fg targets. ASharedMemory is the Android
//      replacement (memfd/ashmem backed).
//   3. posix shm_open() -> the one MangoHudfex actually scans. It opens
//      "<shmdir>/<name>" by path, so a readable DIRECTORY ENTRY is mandatory.
//      On glibc shmdir is /dev/shm; the termux glibc build patches SHMDIR to
//      $PREFIX_CLASSICAL/tmp/ (gpkg/glibc/set-dirs.patch), which is why the
//      reader also probes those paths. This must be tried BEFORE memfd.
//   4. desktop: memfd_create() -> anonymous, no filesystem pollution, but it
//      has NO path: the only handle is /proc/<pid>/fd/<n>, so an out-of-process
//      overlay cannot find it by scanning a directory. Fallback only.
//
// The segment is a single struct with fixed layout (versioned). Counters are
// plain uint64_t written under the layer's global mutex (the present hook is
// serialised), so no lock-free machinery is needed on the writer side; the reader
// treats them as weakly-consistent snapshots (a torn read across two counters is
// benign — it resolves on the next frame).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#  include <android/sharedmem.h>
#else
#  include <sys/syscall.h>
#endif
#include "log.hpp"

namespace winfg_stats {
constexpr uint32_t kMagic   = 0x57464753u;  // "WFGS"
// MUST stay 1. Readers do a strict equality check (MangoHudfex src/winfg.h has
// `constexpr uint32_t kVersion = 1;` and rejects the segment on any mismatch),
// and the struct layout below has never changed, so there is nothing a bump
// would signal. Bumping this to 2 breaks the HUD on every existing overlay.
constexpr uint32_t kVersion = 1;

// Fixed-layout block shared with the overlay. Keep this ABI stable.
struct Stats {
    uint32_t magic;           // kMagic, sanity check on the reader side
    uint32_t version;         // kVersion
    uint64_t real_frames;     // cumulative REAL presents (guest/app frames)
    uint64_t gen_frames;      // cumulative GENERATED presents (win-fg inserted)
    uint64_t last_ns;         // CLOCK_MONOTONIC timestamp of the last update
    uint32_t enabled;         // 1 if frame generation is active
    uint32_t multiplier;      // configured multiplier (2 = 2x, etc.)
};

namespace detail {

enum class Backend : int { kNone = 0, kFile = 1, kAshmem = 2, kMemfd = 3, kShm = 4 };

inline Backend g_backend = Backend::kNone;

inline int envDir(char* out, size_t cap) {
    const char* dir = std::getenv("WIN_FG_STATS_DIR");
    if (!dir || !dir[0]) dir = std::getenv("WIN_FG_SHM_DIR");
    if (!dir || !dir[0]) return 0;
    std::snprintf(out, cap, "%s", dir);
    return 1;
}

// Create the segment and return a fd ready to mmap. Returns -1 on failure.
inline int openSegment(const char* name, size_t size, Backend& tag) {
    tag = Backend::kNone;

    char dir[400];
    if (envDir(dir, sizeof dir)) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/%s", dir, name);
        int fd = ::open(path, O_RDWR | O_CREAT, 0600);
        if (fd >= 0) {
            ::fchmod(fd, 0600);
            if (::ftruncate(fd, (off_t)size) != 0) { ::close(fd); return -1; }
            tag = Backend::kFile;
            WFG_LOGI("stats: publishing to file '%s'", path);
            return fd;
        }
        WFG_LOGE("stats: cannot open '%s' (errno=%d)", path, errno);
        // An explicit dir that is not writable should not disable telemetry
        // outright when a real backend is still available — fall through.
    }

#if defined(__ANDROID__)
#  if defined(__ANDROID_API__) && __ANDROID_API__ >= 26
    {
        int fd = ASharedMemory_create(name, size);
        if (fd >= 0) {
            tag = Backend::kAshmem;
            WFG_LOGI("stats: publishing via ASharedMemory '%s' (fd=%d)", name, fd);
            return fd;
        }
        WFG_LOGE("stats: ASharedMemory_create('%s') failed (errno=%d)", name, errno);
    }
#  else
    WFG_LOGE("stats: NDK API level < 26, no ASharedMemory — set WIN_FG_STATS_DIR to a "
             "writable directory to enable the file backend");
#  endif
    return -1;   // Bionic has no shm_open; nothing else to try.
#else
    {
        // POSIX shm first: it leaves a directory entry under <shmdir>, which is
        // the ONLY way an out-of-process overlay (MangoHudfex scans the shm dir
        // for "win-fg-*-stats") can discover the segment. memfd would "succeed"
        // here just fine but be invisible to it.
        //
        // POSIX also requires the leading '/' in the shm name; glibc accepts
        // both forms, but musl and the name-vs-path logic in some loaders do
        // not, so normalise here and keep `name` (without slash) for the
        // file/ASharedMemory backends and for logging.
        char shm_name[72];
        std::snprintf(shm_name, sizeof shm_name, "/%s",
                      name[0] == '/' ? name + 1 : name);

        // Try the standard shm dir, then the termux-glibc patched one. On
        // termux, SHMDIR is $PREFIX_CLASSICAL/tmp/ (see gpkg/glibc/
        // set-dirs.patch), so shm_open() creates the entry there — the plain
        // shm_open call below already lands in the right place; the explicit
        // fallback only matters when the process runs under a different
        // prefix than the layer was linked against.
        int fd = ::shm_open(shm_name, O_RDWR | O_CREAT, 0600);
        if (fd >= 0) {
            ::fchmod(fd, 0600);
            if (::ftruncate(fd, (off_t)size) == 0) {
                tag = Backend::kShm;
                WFG_LOGI("stats: publishing via shm_open '%s'", shm_name);
                return fd;
            }
            int saved = errno;
            ::close(fd);
            ::shm_unlink(shm_name);
            errno = saved;
        }
        WFG_LOGE("stats: shm_open('%s') failed (errno=%d)", shm_name, errno);
    }
    {
        // Last resort: anonymous memfd. Works, but has no directory entry, so
        // only an overlay that receives the pid and reads /proc/<pid>/fd can
        // find it. Better than nothing.
        int fd = (int)::syscall(SYS_memfd_create, name, 0u /* MFD_CLOEXEC */);
        if (fd >= 0) {
            if (::ftruncate(fd, (off_t)size) == 0) {
                tag = Backend::kMemfd;
                WFG_LOGI("stats: publishing via memfd '%s' (fd=%d, no shm dir entry — "
                         "overlays that scan the shm dir will NOT see this)", name, fd);
                return fd;
            }
            ::close(fd);
        }
    }
    WFG_LOGE("stats: no backend available for '%s' (errno=%d) — overlay will not see "
             "win-fg fps; set WIN_FG_STATS_DIR to a writable directory", name, errno);
    return -1;
#endif
}

} // namespace detail

// Lazy singleton: opened on first account() and kept for the process lifetime.
// Returns nullptr if no backend could be set up (non-fatal: FG still works, the
// overlay simply won't see win-fg stats).
inline Stats* ensure() {
    static Stats* s = nullptr;
    static bool tried = false;
    if (tried) return s;
    tried = true;

    // pid-scoped so a single device can run several games, each with its own
    // win-fg stats segment, and the overlay can match by pid.
    char name[64];
    std::snprintf(name, sizeof name, "win-fg-%d-stats", (int)getpid());

    detail::Backend tag = detail::Backend::kNone;
    const size_t bytes = sizeof(Stats);
    int fd = detail::openSegment(name, bytes, tag);
    if (fd < 0) return nullptr;

    void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);  // the mapping keeps the segment alive
    if (base == MAP_FAILED) {
        WFG_LOGE("stats: mmap failed (errno=%d)", errno);
        return nullptr;
    }
    s = static_cast<Stats*>(base);
    s->magic       = kMagic;
    s->version     = kVersion;
    s->real_frames = 0;
    s->gen_frames  = 0;
    s->last_ns     = 0;
    s->enabled     = 0;
    s->multiplier  = 0;
    detail::g_backend = tag;
    return s;
}

// Human-readable description of where the segment lives. Never null.
inline const char* backendName() {
    switch (detail::g_backend) {
        case detail::Backend::kFile:   return "file";
        case detail::Backend::kAshmem: return "ashmem";
        case detail::Backend::kMemfd:  return "memfd";
        case detail::Backend::kShm:    return "shm_open";
        default:                       return "none";
    }
}

// Call from the present hook. `real` and `gen` are deltas since the last call
// (1 real per guest present, +1 gen when a generated frame was inserted).
inline void account(uint64_t real, uint64_t gen, bool enabled, uint32_t multiplier,
                    uint64_t now_ns) {
    Stats* s = ensure();
    if (!s) return;
    s->real_frames += real;
    s->gen_frames  += gen;
    s->last_ns      = now_ns;
    s->enabled      = enabled ? 1u : 0u;
    s->multiplier   = multiplier;
}

// One-shot "telemetry is live via <backend>" probe for the startup log.
inline void logOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    if (ensure()) WFG_LOGI("stats: live via %s (version %u)", backendName(), (unsigned)kVersion);
}

} // namespace winfg_stats