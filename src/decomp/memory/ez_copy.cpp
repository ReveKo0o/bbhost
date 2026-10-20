// The engine's parallel copy of a resource's file data (docs/decomp.md):
// sub_23bde30 at Binary Ninja 0x23bde30. Its one caller, sub_2157f40 (the
// job a streamed GX resource is built in), hands it the data the file
// repository loaded and a buffer from the graphics heap; it splits the copy
// into page-aligned pieces for the CSEzWorkPool workers and returns a work
// group, and sub_2157c70 - on the main loop's thread, in
// SprjFileStep::STEP_Update - waits for that group before it queues the job
// itself. On the PS4 the destination is write-combined memory a core writes
// slowly, which is what the spread is for; here it is ordinary memory, and
// the wait was the pool's round trip: the main loop's longest waits while
// the world streams (7-27 ms each, ~160 ms a Central Yharnam warp tour with
// the write-watch faults already gone, ~800-1,000 ms before).
//
// Ours copies at once on the calling thread - the destination's watched
// pages released first, as decomp/memory/game_memcpy.cpp does - and hands back no
// group, which the caller already takes for "nothing to wait for": the job
// is queued as before, its data in place.
#include "decomp/decomp.h"
#include "decomp/guest.h"

#include "core/write_watch.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "log.h"

using namespace decomp;

namespace {

// push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; sub rsp, 0x48
// (17 bytes; a rip-relative load follows).
constexpr std::uint8_t kEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                   0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};

std::atomic<std::uint64_t> g_copies{0}, g_bytes{0}, g_largest{0};

// (group out, -, pool, destination, source, bytes) -> the group out. The
// game's version leaves *group 0 when any of the three is 0, as ours does.
DECOMP_LEAF std::uint64_t* ez_copy(std::uint64_t* group, std::uint64_t, std::uint64_t, void* dst, const void* src, std::size_t n) {
    *group = 0;
    if (!dst || !src || !n) return group;
    write_watch_release(dst, n);
    std::memmove(dst, src, n);
    g_copies.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
    std::uint64_t largest = g_largest.load(std::memory_order_relaxed);
    while (n > largest && !g_largest.compare_exchange_weak(largest, n, std::memory_order_relaxed)) {
    }
    return group;
}

void report() {
    host_log("decomp: the engine's parallel copies of streamed resource data done in place: %llu, %llu MiB, the largest %llu KiB",
             static_cast<ull>(g_copies.load()), static_cast<ull>(g_bytes.load() >> 20), static_cast<ull>(g_largest.load() >> 10));
}

}  // namespace

void decomp_ez_copy_add() {
    DecompFunction fn{"sub_23bde30", "memory", 0x23bde30, kEntry, sizeof(kEntry), reinterpret_cast<void*>(&ez_copy),
                      DecompKind::Leaf};
    fn.report = &report;
    decomp_add(fn);
}
