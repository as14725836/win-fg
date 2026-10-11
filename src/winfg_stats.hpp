// win-fg — frame-generation statistics exposed to overlays (MangoHud, etc.)
//
// win-fg runs as a Vulkan implicit layer. An overlay such as MangoHudfex needs to
// show the *post*-interpolation frame rate (real + generated), but it may sit on
// either side of win-fg in the layer chain, so it cannot simply count presents
// itself. Instead win-fg publishes per-frame counters into a small POSIX shared
// memory segment (shm_open), which the overlay reads every frame.
//
// Shared-memory name: "win-fg-<pid>-stats". Placement follows the same rules as
// MangoHudfex's FEX stats reader: Linux defaults shm_open to /dev/shm, but native
// Termux / termux-glibc may lack a writable /dev/shm, so we prefer an explicit
// directory when the caller knows one (see WIN_FG_SHM_DIR), then fall back to
// the C library default (SHMDIR). The reader side scans the same candidate dirs.
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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.hpp"

namespace winfg_stats {

constexpr uint32_t kMagic     = 0x57464753u;  // "WFGS"
constexpr uint32_t kVersion   = 1;

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

// Lazy singleton: opened on first account() and kept for the process lifetime.
// Returns nullptr if the shm could not be created (non-fatal: FG still works,
// the overlay simply won't see win-fg stats).
Stats* ensure() {
    static Stats* s = nullptr;
    if (s) return s;

    // Name is pid-scoped so a single device can run several games, each with its
    // own win-fg stats segment, and the overlay can match by pid.
    char name[64];
    std::snprintf(name, sizeof(name), "win-fg-%d-stats", (int)getpid());

    // Optional explicit shm dir (termux-glibc: $PREFIX/glibc/tmp/shm). If set,
    // open "<dir>/<name>" as a plain file-backed mapping; otherwise let shm_open
    // use the C library default.
    const char* dir = std::getenv("WIN_FG_SHM_DIR");
    int fd = -1;
    if (dir && dir[0]) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s", dir, name);
        fd = open(path, O_RDWR | O_CREAT, 0600);
        if (fd >= 0) std::fchmod(fd, 0600);
    } else {
        fd = shm_open(name, O_RDWR | O_CREAT, 0600);
    }

    if (fd < 0) {
        WFG_LOGE("stats: cannot create shm '%s' (errno=%d) — overlay will not see win-fg fps",
                 name, errno);
        return nullptr;
    }

    if (ftruncate(fd, (off_t)sizeof(Stats)) != 0) {
        WFG_LOGE("stats: ftruncate failed (errno=%d)", errno);
        close(fd);
        return nullptr;
    }

    void* base = mmap(nullptr, sizeof(Stats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);  // mapping keeps the segment alive
    if (base == MAP_FAILED) {
        WFG_LOGE("stats: mmap failed (errno=%d)", errno);
        return nullptr;
    }

    s = static_cast<Stats*>(base);
    // First-touch init (only when freshly created). We don't bother checking
    // whether a stale segment already had magic set: ftruncate + our magic write
    // below is idempotent and the counters start at 0 each process anyway.
    s->magic       = kMagic;
    s->version     = kVersion;
    s->real_frames = 0;
    s->gen_frames  = 0;
    s->last_ns     = 0;
    s->enabled     = 0;
    s->multiplier  = 0;
    return s;
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

} // namespace winfg_stats
