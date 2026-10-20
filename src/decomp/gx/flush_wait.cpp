// The render path's wait for the GPU to finish a flush (docs/decomp.md): GX
// counts the flushes it submits (context +0x3400) and the command processor
// writes back the last one done (the u64 behind the pointer at context
// +0xc30). Before building more, the render path waits until the flush before
// the current one is done - sub_15d7030 at Binary Ninja 0x15d7030, "== Stall
// during rendering at flush %lu" its message. The game's version spins on the
// counter with no pause at all, and in the frame an area first draws the main
// loop spun there for ~180 ms (sampled) - a whole core,
// on a machine with few of them one the command processor it waits for
// needs. Ours is the same wait, polite: a short pause-spin, then the core
// given back while the counter has not moved.
#include "decomp/decomp.h"
#include "decomp/guest.h"

#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

using namespace decomp;
#endif

namespace {

std::atomic<std::uint64_t> g_waits{0}, g_waited_us{0}, g_longest_us{0};

// The entry's first whole instructions: push rbp; mov rbp, rsp; push r15,
// r14, r12, rbx; sub rsp, 0x10 (15 bytes; its rip-relative load follows).
constexpr std::uint8_t kEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x10};

GUEST_ABI void flush_wait(std::uint64_t ctx) {
    const std::uint64_t submitted = *reinterpret_cast<const volatile std::uint64_t*>(static_cast<std::uintptr_t>(ctx + 0x3400));
    if (!submitted) return;
    const auto* done = *reinterpret_cast<const volatile std::uint64_t* const*>(static_cast<std::uintptr_t>(ctx + 0xc30));
    const std::uint64_t want = submitted - 1;
    if (*done >= want) return;
    const auto t0 = std::chrono::steady_clock::now();
    bool said = false;
    for (unsigned i = 0; *done < want; ++i) {
        if (i < 2048) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
        } else {
            std::this_thread::yield();
            // The game prints its line after ten million spins; ours says
            // once, after a second, where the wait is.
            if (!said && (i & 1023) == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(1)) {
                said = true;
                host_log("decomp: the render path has waited a second for flush %llu (done %llu)", static_cast<ull>(want),
                         static_cast<ull>(*done));
            }
        }
    }
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    g_waits.fetch_add(1, std::memory_order_relaxed);
    g_waited_us.fetch_add(us, std::memory_order_relaxed);
    std::uint64_t longest = g_longest_us.load(std::memory_order_relaxed);
    while (us > longest && !g_longest_us.compare_exchange_weak(longest, us, std::memory_order_relaxed)) {
    }
}

void report() {
    if (!g_waits.load()) return;
    host_log("decomp: the render path waited for a flush %llu times, %llu ms in all, the longest %llu ms",
             static_cast<ull>(g_waits.load()), static_cast<ull>(g_waited_us.load() / 1000), static_cast<ull>(g_longest_us.load() / 1000));
}

}  // namespace

void decomp_gx_flush_wait_add() {
    DecompFunction fn{"sub_15d7030", "gx", 0x15d7030, kEntry, sizeof(kEntry), reinterpret_cast<void*>(&flush_wait),
                      DecompKind::Hosted};
    fn.report = &report;
    decomp_add(fn);
}
