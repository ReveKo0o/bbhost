#include "engine/guest.h"

#include "core/elf.h"
#include "engine/sprj_flipper.h"
#include "hle/modules.h"
#include "log.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

ElfImage* g_image = nullptr;
std::atomic<int> g_flipper_logs{0};

void* host_from_guest(std::uint64_t va, std::size_t n) {
    if (!g_image || n == 0) {
        return nullptr;
    }
    const auto& mem = g_image->mem;
    if (va >= mem.slide && n <= mem.size && va - mem.slide <= mem.size - n) {
        return guest_ptr(mem, va);
    }
    if (hle_kernel_va_mapped(va, n)) {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(va));
    }
    return nullptr;
}

}  // namespace

void engine_bind(ElfImage* image) {
    g_flipper_logs.store(0);
    if (!image) {
        g_image = nullptr;
        return;
    }
    if (image->sha256 != kEboot109Sha256) {
        host_log("engine: eboot is not 1.09; named layouts skipped");
        g_image = nullptr;
        return;
    }
    g_image = image;
    host_log("engine: bound 1.09 image slide=0x%llx SprjFlipper slot elf=0x%llx",
             static_cast<unsigned long long>(image->mem.slide),
             static_cast<unsigned long long>(kSprjFlipperSingletonElf));
}

void engine_on_flip(std::uint64_t flip_count) {
    if (!g_image || g_flipper_logs.load() != 0) {
        return;
    }
    const std::uint64_t slot_va = guest_from_elf(kSprjFlipperSingletonElf, g_image->mem.slide);
    auto* slot = static_cast<std::uint64_t*>(host_from_guest(slot_va, sizeof(std::uint64_t)));
    if (!slot || *slot == 0) {
        return;
    }
    auto* f = static_cast<SprjFlipper*>(host_from_guest(*slot, sizeof(SprjFlipper)));
    if (!f) {
        return;
    }
    if (g_flipper_logs.fetch_add(1) != 0) {
        return;
    }
    host_log("engine: SprjFlipper obj=0x%llx flip=%llu mode=%u/%u target=%.4fs measured=%.4fs fps=%.2f skip=%u",
             static_cast<unsigned long long>(*slot),
             static_cast<unsigned long long>(flip_count),
             f->primary_flip_mode_raw, f->secondary_flip_mode_raw,
             static_cast<double>(f->target_frame_seconds),
             static_cast<double>(f->measured_frame_seconds),
             static_cast<double>(f->calculated_fps),
             f->allow_frame_skip);
}
