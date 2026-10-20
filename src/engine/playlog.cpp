#include "engine/playlog.h"

#include "core/config.h"
#include "core/elf.h"
#include "core/memory.h"
#include "engine/addr.h"
#include "log.h"

#include <cstdint>
#include <cstring>

namespace {
// The initializer's stores, `mov dword [rip+disp], imm32`; the last four bytes
// are the float. Both sit in one page of 0x23f9ad0.
// 0x23f9b86: 300.0f into the upload period (0x5a9ecc0).
constexpr std::uint64_t kUploadStore = 0x23f9b86;  // Binary Ninja (guest VA)
constexpr std::uint8_t kUploadExpect[] = {0xc7, 0x05, 0x30, 0x51, 0x6a, 0x03, 0x00, 0x00, 0x96, 0x43};
// 0x23f9b68: 1.5f into the position (RegularLog) period (0x5a9eca8).
constexpr std::uint64_t kSampleStore = 0x23f9b68;
constexpr std::uint8_t kSampleExpect[] = {0xc7, 0x05, 0x36, 0x51, 0x6a, 0x03, 0x00, 0x00, 0xc0, 0x3f};

std::uint8_t* store_at(GuestMemory& mem, std::uint64_t va, const std::uint8_t* expect, std::size_t n) {
    const std::uint64_t off = va - kPreferredGuestSlide;
    if (off + n > mem.size) return nullptr;
    auto* p = static_cast<std::uint8_t*>(guest_ptr(mem, mem.slide + off));
    if (std::memcmp(p, expect, n) != 0) {
        host_log("playlog: not patched: unexpected bytes at 0x%llx", static_cast<unsigned long long>(va));
        return nullptr;
    }
    return p;
}
}  // namespace

void playlog_install(ElfImage* image) {
    const int seconds = config().playlog_upload_seconds;
    const int sample_ms = config().playlog_sample_ms;
    if (seconds <= 0 && sample_ms <= 0) return;    // the game's own 300 s and 1.5 s
    GuestMemory& mem = image->mem;
    std::uint8_t* upload = seconds > 0 ? store_at(mem, kUploadStore, kUploadExpect, sizeof(kUploadExpect)) : nullptr;
    std::uint8_t* sample = sample_ms > 0 ? store_at(mem, kSampleStore, kSampleExpect, sizeof(kSampleExpect)) : nullptr;
    if (!upload && !sample) return;
    const std::uint64_t page = (mem.slide + kSampleStore - kPreferredGuestSlide) & ~0xfffull;
    if (!guest_protect_rwx(&mem, page, 0x2000)) {
        host_log("playlog: cannot unprotect the initializer page");
        return;
    }
    if (upload) {
        const float period = static_cast<float>(seconds);
        std::memcpy(upload + 6, &period, sizeof(period));
        host_log("playlog: the game uploads its play log every %d s (was 300)", seconds);
    }
    if (sample) {
        const float period = static_cast<float>(sample_ms) / 1000.0f;
        std::memcpy(sample + 6, &period, sizeof(period));
        host_log("playlog: the game logs the player's position every %d ms (was 1500)", sample_ms);
    }
    guest_protect_rx(&mem, page, 0x2000);
}
