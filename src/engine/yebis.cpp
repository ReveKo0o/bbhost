#include "engine/yebis.h"

#include "core/elf.h"
#include "engine/addr.h"
#include "hle/modules.h"
#include "log.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {

// sub_25fcf20 reaches the debug-menu tree through this pointer, and
// sub_26979b0 builds the graphics manager at <that object> + 0x5860. The
// manager holds the YEBIS settings at +0x110 (sub_261a8c0 allocates it there
// and passes it to the builder as sub_25fcf20(*(manager + 0x110))).
constexpr std::uint64_t kRootPtr = 0x5940dd8;
constexpr std::uint32_t kManagerOff = 0x5860;
constexpr std::uint32_t kSettingsOff = 0x110;
// The block the debug nodes edit is a copy of one 0x68 earlier.
constexpr std::uint32_t kBaseDelta = 0x68;

GuestMemory g_mem;
bool g_installed = false;
void* g_settings = nullptr;
bool g_looked = false;

bool mapped(std::uint64_t va, std::size_t n) {
    return va >= g_mem.slide && va + n <= g_mem.slide + g_mem.size;
}

std::uint64_t rd64(std::uint64_t va) {
    std::uint64_t v = 0;
    std::memcpy(&v, guest_ptr(g_mem, va), sizeof(v));
    return v;
}

std::int32_t rd32_at(const void* p, std::uint32_t off) {
    std::int32_t v = 0;
    std::memcpy(&v, static_cast<const std::uint8_t*>(p) + off, sizeof(v));
    return v;
}

// The constructor's defaults, which is what says "this really is the object"
// rather than "this pointer chain did not fault". sub_25fc6f0 writes
// LensDistortion 1, ApertureairyDisc 0, MotionBlur-Dof 1, Gaussian 0,
// LightShaft 1, ColorGrading 0.
bool looks_right(const void* p) {
    struct Check {
        std::uint32_t off;
        std::int32_t want;
    };
    static const Check checks[] = {
        {0xd4, 1}, {0xd8, 0}, {0xe0, 1}, {0xec, 0}, {0xf8, 1}, {0x108, 0},
    };
    for (const Check& c : checks) {
        const std::int32_t got = rd32_at(p, c.off);
        if (got != c.want) {
            host_log("yebis: +0x%x is %d, expected the constructor's %d - not the settings object",
                     c.off, got, c.want);
            return false;
        }
    }
    return true;
}

}  // namespace

void yebis_install(ElfImage* image) {
    g_mem = image->mem;
    g_installed = true;
}

void* yebis_settings() {
    if (!g_installed || g_looked) {
        return g_settings;
    }
    const std::uint64_t ptr_va = g_mem.slide + (kRootPtr - kPreferredGuestSlide);
    if (!mapped(ptr_va, 8)) {
        return nullptr;
    }
    const std::uint64_t root = rd64(ptr_va);
    if (!root) {
        return nullptr;  // not built yet; try again next time
    }
    // **Retry.** The first cut latched as soon as the root pointer was
    // non-null and reported "no settings object" forever after, because the
    // graphics manager is built later than the root. Give up only after a
    // bounded number of attempts, and say what was seen on the way.
    static int tries = 0;
    if (++tries > 20000) {
        return nullptr;
    }
    const std::uint64_t manager = root + kManagerOff;
    std::uint64_t settings = 0;
    std::memcpy(&settings, reinterpret_cast<void*>(static_cast<std::uintptr_t>(manager + kSettingsOff)),
                sizeof(settings));
    if (!settings) {
        static int logs = 0;
        if (logs < 3) {
            ++logs;
            host_log("yebis: root 0x%llx, manager +0x%x, no settings at +0x%x yet (try %d)",
                     static_cast<unsigned long long>(root), kManagerOff, kSettingsOff, tries);
        }
        return nullptr;
    }
    g_looked = true;
    void* p = reinterpret_cast<void*>(static_cast<std::uintptr_t>(settings));
    if (!looks_right(p)) {
        host_log("yebis: candidate at %p (root 0x%llx) did not match the constructor's defaults", p,
                 static_cast<unsigned long long>(root));
        return nullptr;
    }
    g_settings = p;
    // Note for anyone following sub_25d9ea0: it loads the param file into
    // `*(arg1 + 0x18) + 0x110`, which is **not** this object - both happen to
    // keep something at +0x110. Dereferencing this one's +0x48/+0x50 as if
    // they were the loaded block reads a guest-image address and then
    // segfaults, which is how that was established.
    host_log("yebis: settings at %p (root 0x%llx + 0x%x + 0x%x); MotionBlur-Dof=%d DofQuality=%d "
             "GlareQuality=%d LightShaft=%d",
             p, static_cast<unsigned long long>(root), kManagerOff, kSettingsOff,
             rd32_at(p, kYebisMotionBlurDof), rd32_at(p, kYebisDofQuality),
             rd32_at(p, kYebisGlareQuality), rd32_at(p, kYebisLightShaft));
    return p;
}

bool yebis_get(std::uint32_t off, std::int32_t* out) {
    void* p = yebis_settings();
    if (!p) {
        return false;
    }
    *out = rd32_at(p, off);
    return true;
}

bool yebis_set(std::uint32_t off, std::int32_t value) {
    void* p = yebis_settings();
    if (!p) {
        return false;
    }
    // Both blocks: the debug copy and the base it was copied from. Which one
    // the renderer reads has not been settled, and writing the same value to
    // both is correct either way - they are one setting, not two.
    std::memcpy(static_cast<std::uint8_t*>(p) + off, &value, sizeof(value));
    if (off >= kBaseDelta) {
        std::memcpy(static_cast<std::uint8_t*>(p) + off - kBaseDelta, &value, sizeof(value));
    }
    return true;
}

namespace {

struct Forced {
    std::uint32_t off;
    std::int32_t value;
};
Forced g_forced[16];
int g_forced_count = 0;

}  // namespace

void yebis_poll() {
    static const bool parsed = [] {
        const char* e = std::getenv("BBHOST_YEBIS_SET");
        while (e && *e && g_forced_count < 16) {
            char* end = nullptr;
            const unsigned long off = std::strtoul(e, &end, 0);
            if (end == e || *end != '=') {
                break;
            }
            e = end + 1;
            const long v = std::strtol(e, &end, 0);
            if (end == e) {
                break;
            }
            g_forced[g_forced_count++] = {static_cast<std::uint32_t>(off), static_cast<std::int32_t>(v)};
            e = *end == ',' ? end + 1 : end;
        }
        return true;
    }();
    (void)parsed;
    // Resolve first and unconditionally: the previous order returned before
    // looking whenever nothing was being forced, so a plain run reported
    // nothing at all and looked like the path had broken.
    if (!yebis_settings() || !g_forced_count) {
        return;
    }
    // BBHOST_YEBIS_AT_FLIP=N holds off until then, so one run can dump a frame
    // before and after the change - the same scene either side, which a pair
    // of runs cannot give.
    static const std::uint64_t at_flip = [] {
        const char* e = std::getenv("BBHOST_YEBIS_AT_FLIP");
        return e ? std::strtoull(e, nullptr, 0) : 0ull;
    }();
    if (at_flip && hle_video_flip_count() < at_flip) {
        return;
    }
    static int logs = 0;
    for (int i = 0; i < g_forced_count; ++i) {
        std::int32_t now = 0;
        if (yebis_get(g_forced[i].off, &now) && now != g_forced[i].value) {
            yebis_set(g_forced[i].off, g_forced[i].value);
            if (logs < 8) {
                ++logs;
                host_log("yebis: +0x%x was %d, forced to %d", g_forced[i].off, now, g_forced[i].value);
            }
        }
    }
}
