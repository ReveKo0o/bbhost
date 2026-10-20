#include "engine/menu_steps.h"

#include "core/elf.h"
#include "core/thunk.h"
#include "engine/addr.h"
#include "engine/graphics_patch.h"
#include "log.h"

#include <atomic>
#include <cstring>

namespace {

constexpr std::uint64_t kStepCtor = 0x201cce0;  // (step, ctx, type, name, factory, ...)
constexpr std::uint8_t kStepPrologue[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x53, 0x50, 0x4d, 0x89, 0xce, 0x48, 0x89, 0xfb};

struct Watch {
    std::uint64_t type = 0;
    const char16_t* name = nullptr;
    std::atomic<std::uint64_t> made{0};
};
constexpr int kMaxWatches = 8;
Watch g_watches[kMaxWatches];
std::atomic<int> g_count{0};
int g_installed = -1;  // -1 not tried, 0 failed, 1 hooked

// saved[] is r9, r8, rcx, rdx, rsi, rdi: the step rdi, its type rdx, its name rcx.
GUEST_ABI std::int64_t step_hook(std::uint64_t, const std::uint64_t* saved) {
    const auto* name = reinterpret_cast<const char16_t*>(static_cast<std::uintptr_t>(saved[2]));
    if (!name) return 0;
    const int n = g_count.load(std::memory_order_acquire);
    for (int w = 0; w < n; ++w) {
        Watch& x = g_watches[w];
        if (saved[3] != x.type) continue;
        int i = 0;
        while (x.name[i] && name[i] == x.name[i]) ++i;
        if (!x.name[i] && !name[i]) x.made.store(saved[5], std::memory_order_release);
    }
    return 0;
}

std::uint64_t rd64(std::uint64_t a) {
    std::uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof v);
    return v;
}

}  // namespace

int menu_steps_watch(std::uint64_t type, const char16_t* name) {
    const int w = g_count.load(std::memory_order_relaxed);
    if (w >= kMaxWatches || !name) return -1;
    g_watches[w].type = type;
    g_watches[w].name = name;
    g_count.store(w + 1, std::memory_order_release);
    return w;
}

bool menu_steps_install(ElfImage* image) {
    if (g_installed >= 0) return g_installed == 1;
    g_installed = 0;
    if (!image || image->sha256 != kEboot109Sha256) return false;
    const std::uint64_t at = image->mem.slide + (kStepCtor - kPreferredGuestSlide);
    if (engine_prologue_hook(image, at, kStepPrologue, sizeof(kStepPrologue), reinterpret_cast<void*>(&step_hook))) g_installed = 1;
    else host_log("menu steps: the step constructor is not as expected; menus are not watched");
    return g_installed == 1;
}

std::uint64_t menu_steps_take(int watch) {
    if (watch < 0 || watch >= g_count.load(std::memory_order_acquire)) return 0;
    return g_watches[watch].made.exchange(0, std::memory_order_acq_rel);
}

void menu_step_hold(std::uint64_t step) { ++*menu_step_count(step); }

void menu_step_release(std::uint64_t step) {
    if (--*menu_step_count(step) == 0)
        hle_call_guest<std::int64_t>(reinterpret_cast<void*>(static_cast<std::uintptr_t>(rd64(rd64(step)))), step);
}
