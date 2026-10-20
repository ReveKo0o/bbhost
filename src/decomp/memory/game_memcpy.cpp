// The eboot's own memcpy (docs/decomp.md): sub_2a1a1b0 at Binary Ninja
// 0x2a1a1b0, 1,440 call sites - GX's resource creators copying a new
// buffer's or texture's initial data in (sub_256d420 at 0x256d5ea,
// sub_2571d60), the work items of the engine's parallel copy of a streamed
// file (sub_23bde30, which the main loop waits for in sub_2157c70), and the
// struct and string copies of everything else.
//
// Ours copies with the host's memmove, and a copy of kReleaseBytes or more
// first releases the destination's write-watched pages in one call
// (core/write_watch.h): memory a dead resource's surface or a buffer-shadow
// page still watched faulted once per 4 KiB page under the game's copy - on
// the main loop's thread, or on the pool workers it was waiting for. In a
// Central Yharnam warp tour, after a fault learned to release the pages
// ahead of a streaming writer, texture creation's copies were still 4,843
// of the last 16,384 faults: too short for a run to build up. The copy
// writes the whole range, so a release before it is what its faults would
// have done, a page at a time.
//
// The game's version does not return the destination (its small-size path
// leaves a jump-table address in rax), so no caller reads the result; where
// source and destination overlap it copies forward in blocks, memmove
// exactly. There is no compare run: a copy is checked by what it copies.
#include "decomp/decomp.h"
#include "decomp/guest.h"

#include "core/write_watch.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "log.h"

using namespace decomp;

namespace {

// cmp rdx, 0x201; jb +0x29; mov rax, rdi; and rax, 0xf (16 bytes, whole
// instructions; the jb is why `original` stays unset).
constexpr std::uint8_t kEntry[] = {0x48, 0x81, 0xfa, 0x01, 0x02, 0x00, 0x00, 0x72,
                                   0x29, 0x48, 0x89, 0xf8, 0x48, 0x83, 0xe0, 0x0f};

// Below this a copy is a few pages at most, and a release's page scan and
// protect call would cost more than the copy saves.
constexpr std::size_t kReleaseBytes = 16 * 1024;

std::atomic<std::uint64_t> g_big{0}, g_released{0};

DECOMP_LEAF void game_memcpy(void* dst, const void* src, std::size_t n) {
    if (n >= kReleaseBytes) {
        g_big.fetch_add(1, std::memory_order_relaxed);
        if (write_watch_release(dst, n)) g_released.fetch_add(1, std::memory_order_relaxed);
    }
    std::memmove(dst, src, n);
}

void report() {
    host_log("decomp: the game's memcpy: %llu copies of %zu KiB or more, %llu of them into write-watched pages released first",
             static_cast<ull>(g_big.load()), kReleaseBytes / 1024, static_cast<ull>(g_released.load()));
}

}  // namespace

void decomp_game_memcpy_add() {
    DecompFunction fn{"sub_2a1a1b0", "memory", 0x2a1a1b0, kEntry, sizeof(kEntry), reinterpret_cast<void*>(&game_memcpy),
                      DecompKind::Leaf};
    fn.report = &report;
    decomp_add(fn);
}
