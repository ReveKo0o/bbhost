#include "guest_abi.h"
#include "engine/fmod_probe.h"

#include "core/elf.h"
#include "engine/addr.h"
#include "engine/graphics_patch.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

// FMOD Ex 4.44.50's AT9 codec (fmod_codec_at9.cpp in the eboot). 0x1119c00
// takes the channel's pending AJM job: it returns FMOD_ERR_INTERNAL (0x21)
// without waiting when the job was submitted in the current codec tick
// (+0x26c == the tick counter at 0x586d328), and after the wait when a
// setup result (+0x1f0 gapless control, +0x1f8 codec-info run, +0x210
// initialize) is nonzero, the codec info's superframe size (+0x200) is not
// the sound's (+0x238), or the run's result (+0x240) has bits other than
// PARTIAL_INPUT / NOT_ENOUGH_ROOM. Otherwise it adds the run's output
// (+0x24c) to the ring (+0x234) and, when the run reported 0 samples decoded
// (+0x250), marks decoding finished (+0x290 |= 8).
constexpr std::uint64_t kJobDone = 0x1119c00;
constexpr std::uint8_t kJobDonePrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                             0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x30};
constexpr std::uint64_t kTick = 0x586d328;

std::uint64_t g_slide = 0;
std::atomic<std::uint64_t> g_calls{0}, g_fails{0};

template <typename T>
T rd(std::uint64_t va) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), sizeof(v));
    return v;
}

GUEST_ABI std::int64_t job_done_hook(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t c = saved[5];  // rdi: the codec
    const std::uint64_t tick = rd<std::uint64_t>(g_slide + (kTick - kPreferredGuestSlide));
    const std::uint32_t job_tick = rd<std::uint32_t>(c + 0x26c);
    const std::uint64_t pending = rd<std::uint64_t>(c + 0x260);
    const char* why = nullptr;
    char buf[160];
    if (job_tick == tick) {
        why = "the pending job was submitted this tick (not waited on)";
    } else if (rd<std::uint32_t>(c + 0x1f0) || rd<std::uint32_t>(c + 0x1f8) || rd<std::uint32_t>(c + 0x210)) {
        std::snprintf(buf, sizeof(buf), "a setup result is nonzero (gapless %08x, info %08x, init %08x)", rd<std::uint32_t>(c + 0x1f0),
                      rd<std::uint32_t>(c + 0x1f8), rd<std::uint32_t>(c + 0x210));
        why = buf;
    } else if (static_cast<std::int32_t>(rd<std::int16_t>(c + 0x238)) != rd<std::int32_t>(c + 0x200)) {
        std::snprintf(buf, sizeof(buf), "superframe size %d, codec info says %d", rd<std::int16_t>(c + 0x238), rd<std::int32_t>(c + 0x200));
        why = buf;
    } else if (rd<std::uint32_t>(c + 0x240) & 0xffffffe7u) {
        std::snprintf(buf, sizeof(buf), "run result %08x", rd<std::uint32_t>(c + 0x240));
        why = buf;
    }
    const std::uint64_t n = g_calls.fetch_add(1) + 1;
    if (n == 1 || n == 100 || n == 10000) {
        host_log("fmod probe: %llu job completions so far, %llu failing", static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(g_fails.load()));
    }
    if (why) {
        const std::uint64_t f = g_fails.fetch_add(1) + 1;
        if (f <= 200 || (f & (f - 1)) == 0) {
            host_log("fmod probe: AT9 codec %llx (AJM instance %u) job completion fails: %s; ring has %d bytes, pending job %s, "
                     "flags %x, tick %llu job tick %u (%llu of %llu calls failed)",
                     static_cast<unsigned long long>(c), rd<std::uint32_t>(c + 0x1ec), why, rd<std::int32_t>(c + 0x234),
                     pending ? "yes" : "no", rd<std::uint32_t>(c + 0x290), static_cast<unsigned long long>(tick), job_tick,
                     static_cast<unsigned long long>(f), static_cast<unsigned long long>(n));
        }
    }
    return 0;  // the function runs as it would
}

}  // namespace

void fmod_probe_install(ElfImage* image) {
    const char* e = std::getenv("BBHOST_FMOD_PROBE");
    if (!e || *e != '1') return;
    g_slide = image->mem.slide;
    const std::uint64_t at = g_slide + (kJobDone - kPreferredGuestSlide);
    const bool ok = engine_prologue_hook(image, at, kJobDonePrologue, sizeof(kJobDonePrologue), reinterpret_cast<void*>(&job_done_hook));
    host_log("fmod probe: %s on the AT9 codec's job completion (0x%llx)", ok ? "hooked" : "NOT hooked (unexpected bytes)",
             static_cast<unsigned long long>(kJobDone));
}
