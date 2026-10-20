#include "engine/sos_signs.h"

#include "core/config.h"
#include "core/elf.h"
#include "core/memory.h"
#include "engine/addr.h"
#include "log.h"

#include <cstdint>
#include <cstring>

namespace {
constexpr std::uint64_t kSignTimeout = 0x4d27a5c;  // Binary Ninja (guest VA): 180.0f
constexpr float kGameTimeout = 180.0f;
}  // namespace

void sos_signs_install(ElfImage* image) {
    const int seconds = config().sign_timeout_seconds;
    if (!image || image->sha256 != kEboot109Sha256 || seconds <= 0 || seconds == 180) return;
    GuestMemory& mem = image->mem;
    const std::uint64_t at = mem.slide + (kSignTimeout - kPreferredGuestSlide);
    auto* p = static_cast<float*>(guest_ptr(mem, at));
    if (std::memcmp(p, &kGameTimeout, sizeof(float)) != 0) {
        host_log("sos signs: not patched: 0x%llx is not 180.0", static_cast<unsigned long long>(kSignTimeout));
        return;
    }
    if (!guest_protect_rw(&mem, at & ~0xfffull, 0x1000)) {
        host_log("sos signs: cannot unprotect 0x%llx", static_cast<unsigned long long>(kSignTimeout));
        return;
    }
    *p = static_cast<float>(seconds);  // the page stays writable, as frame_rate.cpp leaves .rodata it rewrites
    host_log("sos signs: a sign entry times out after %d s (was 180)", seconds);
}
