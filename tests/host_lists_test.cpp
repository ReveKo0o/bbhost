// The host command lists (host/host_lists.cpp), without the game: command
// buffer objects over chunks of our own memory, packets noted as the writers
// note them, ranges taken as a submission takes them, and walks compared as
// the command processor's shadow compares them - with the chunk written again
// from an earlier place, handed out by the arena, filled past its list's
// room, ranges outside any chunk, packets nothing reads, and many threads
// making and filling lists at once. Runs with BBHOST_HOST_LISTS=2
// (CMakeLists.txt).
#include "host/host_lists.cpp"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <thread>
#include <vector>

using namespace host_lists;

namespace {

int g_failed = 0;
void check(bool ok, const char* what, int line) {
    if (ok) return;
    if (++g_failed <= 40) std::printf("FAIL (line %d): %s\n", line, what);
}
#define CHECK(x) check((x), #x, __LINE__)

// The chunks, carved from one block with gaps between them, so that no two
// tests' chunks share an address (the lists outlive them) and an address in
// a gap is in no chunk.
alignas(4096) std::uint32_t g_block[4u << 20];
std::size_t g_block_used = 0;
std::uint32_t* carve(std::size_t dwords) {
    std::uint32_t* p = g_block + g_block_used;
    g_block_used += (dwords + 16384 + 1023) & ~std::size_t{1023};  // a 64 KiB gap after each
    if (g_block_used > std::size(g_block)) std::abort();
    return p;
}

// A Gnm command buffer object over a chunk: {+0x0 the chunk's start, +0x8
// its end, +0x10 the write pointer}.
struct Buffer {
    alignas(16) std::uint64_t object[6] = {};
    struct Dwords {
        std::uint32_t* p;
        std::size_t n;
        std::uint32_t& operator[](std::size_t k) { return p[k]; }
        const std::uint32_t& operator[](std::size_t k) const { return p[k]; }
        std::size_t size() const { return n; }
        std::uint32_t* data() const { return p; }
    } chunk;
    explicit Buffer(std::size_t dwords) : chunk{carve(dwords + 2), dwords + 2} { reset(); }
    std::uint64_t begin() const { return reinterpret_cast<std::uint64_t>(chunk.data()); }
    std::uint64_t cb() const { return reinterpret_cast<std::uint64_t>(object); }
    void reset() {
        object[0] = begin();
        object[1] = begin() + (chunk.size() - 2) * 4;
        object[2] = begin();
    }
    std::uint64_t at(std::size_t dw) const { return begin() + dw * 4; }
    // A packet at dword `dw`: its header (count = len - 2), the body.
    void put(std::size_t dw, std::uint32_t op, std::size_t len, std::uint32_t first = 0) {
        chunk[dw] = 0xc0000000u | (static_cast<std::uint32_t>(len - 2) << 16) | (op << 8);
        for (std::size_t k = 1; k < len; ++k) chunk[dw + k] = 0;
        if (len > 1) chunk[dw + 1] = first;
    }
    void note(std::size_t dw, std::uint32_t op) { host_lists::note(cb(), at(dw), op); }
};

std::vector<std::uint32_t> entries(const Taken& t) { return t.entries; }
std::uint32_t e(std::uint32_t dw, std::uint32_t op) { return (dw << 8) | op; }

// The walker's view of a range: each packet's header, in order.
void walk(const Buffer& b, std::size_t lo, std::size_t hi, const Taken& t) {
    shadow_begin(&t, b.at(lo), (hi - lo) * 4);
    for (std::size_t dw = lo; dw < hi;) {
        const std::uint32_t hdr = b.chunk[dw];
        if ((hdr >> 30) != 3) {
            ++dw;
            continue;
        }
        const std::size_t n = ((hdr >> 16) & 0x3fff) + 1;
        shadow_packet(b.at(dw), (hdr >> 8) & 0xff, &b.chunk[dw + 1], n);
        dw += 1 + n;
    }
    shadow_end();
}

struct Seen {
    u64 ranges, covered, differed, no_list, overflowed;
};
Seen seen() { return {g.ranges.load(), g.covered.load(), g.differed.load(), g.no_list.load(), g.overflowed.load()}; }

}  // namespace

int main() {
    if (g_mode != 2 || !g_pool) {
        std::printf("host_lists_test: needs BBHOST_HOST_LISTS=2 (got %d)\n", g_mode);
        return 1;
    }

    // Recorded in order, taken by range, places from the range's start.
    {
        Buffer b(4096);
        b.put(0, 0x10, 4, kGxNativeMagic);   // a token
        b.put(4, 0x10, 6, 0x68750001u);      // a marker: not noted
        b.put(10, 0x47, 6);                  // a label
        b.put(16, 0x3c, 7);                  // a wait
        b.put(23, 0x76, 3);                  // SET_SH_REG: not noted
        b.put(26, 0x10, 4, kGxBarrierMagic);
        b.note(0, 0x10);
        b.note(10, 0x47);
        b.note(16, 0x3c);
        b.note(26, 0x10);
        Taken t;
        take(b.at(0), 30 * 4, &t);
        CHECK(t.state == Taken::kTaken);
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(0, 0x10), e(10, 0x47), e(16, 0x3c), e(26, 0x10)}));
        take(b.at(10), 16 * 4, &t);  // [10, 26): the label and the wait
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(0, 0x47), e(6, 0x3c)}));
        take(b.at(30), 8, &t);
        CHECK(t.state == Taken::kTaken && t.entries.empty());

        // The walk of [0, 30) meets what the list holds: covered.
        const Seen s0 = seen();
        take(b.at(0), 30 * 4, &t);
        walk(b, 0, 30, t);
        const Seen s1 = seen();
        CHECK(s1.ranges == s0.ranges + 1 && s1.covered == s0.covered + 1 && s1.differed == s0.differed);
        CHECK(g.matched[0x10].load() >= 2 && g.matched[0x47].load() >= 1 && g.matched[0x3c].load() >= 1);

        // A token nobody noted: the walk differs.
        b.put(30, 0x10, 4, kGxClearMagic);
        take(b.at(0), 34 * 4, &t);
        walk(b, 0, 34, t);
        const Seen s2 = seen();
        CHECK(s2.differed == s1.differed + 1 && s2.covered == s1.covered);
        // Noted where there is a packet nothing reads (the marker): differs.
        b.note(4, 0x10);
        take(b.at(4), 6 * 4, &t);
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(0, 0x10)}));
        walk(b, 4, 10, t);
        CHECK(seen().differed == s2.differed + 1);
    }

    // Written over again (a packet at or before the last one recorded): the
    // list starts over with it.
    {
        Buffer b(1024);
        for (std::size_t dw : {10, 15, 20, 30}) b.put(dw, dw == 30 ? 0x3c : 0x47, 3);
        b.note(10, 0x47);
        b.note(20, 0x47);
        b.note(30, 0x3c);
        b.note(15, 0x47);
        Taken t;
        take(b.at(0), 100 * 4, &t);
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(15, 0x47)}));
        b.put(0, 0x20, 3);
        b.note(0, 0x20);
        b.note(10, 0x47);
        take(b.at(0), 100 * 4, &t);
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(0, 0x20), e(10, 0x47)}));
        b.note(10, 0x47);  // at the last one's place
        take(b.at(0), 100 * 4, &t);
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(10, 0x47)}));
        // An entry whose packet is no longer there (a round that wrote data
        // over it): dropped when taken.
        b.note(12, 0x10);
        b.chunk[12] = 0x12345678u;
        const u64 stale = g_stale.load();
        take(b.at(0), 100 * 4, &t);
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(10, 0x47)}) && g_stale.load() == stale + 1);
    }

    // GX's arena hands the chunk out again: its list starts over.
    {
        Buffer b(1024);
        b.put(2, 0x10, 4, kGxNativeMagic);
        b.put(5, 0x47, 3);
        b.put(9, 0x10, 4, kGxNativeMagic);
        b.note(5, 0x47);
        b.note(9, 0x10);
        chunk_handed_out(b.begin());
        Taken t;
        take(b.at(0), 64, &t);
        CHECK(t.state == Taken::kTaken && t.entries.empty());
        b.note(2, 0x10);
        take(b.at(0), 64, &t);
        CHECK(entries(t) == (std::vector<std::uint32_t>{e(2, 0x10)}));
        chunk_handed_out(b.begin() + 4096 * 64);  // a chunk with no list: nothing
    }

    // A list that fills up stops recording, and its ranges are not covered,
    // until the chunk starts over.
    {
        Buffer b(256);  // room for 256 * 4 / 16 + 64 = 128 entries
        const u64 dwords = 256;
        Region* r = nullptr;
        for (std::uint32_t k = 0; k < dwords; ++k) b.chunk[k] = 0xc0001000u;  // one-dword NOPs: header and op 0x10
        for (std::uint32_t k = 0; k < dwords; ++k) b.note(k, 0x10);
        r = find_region(b.begin());
        CHECK(r && r->cap == dwords * 4 / 16 + 64);
        CHECK(r && (r->flags.load() & kOverflow));
        Taken t;
        take(b.at(0), dwords * 4, &t);
        CHECK(t.state == Taken::kOverflowed);
        const Seen s0 = seen();
        walk(b, 0, 4, t);
        CHECK(seen().overflowed == s0.overflowed + 1);
        b.put(0, 0x47, 6);
        b.note(0, 0x47);  // the chunk from its start again
        take(b.at(0), dwords * 4, &t);
        CHECK(t.state == Taken::kTaken && entries(t) == (std::vector<std::uint32_t>{e(0, 0x47)}));
    }

    // Outside any chunk, or across a chunk's end: no list. A packet outside
    // its command buffer's chunk is not recorded.
    {
        Buffer b(512);
        b.note(1, 0x47);
        Taken t;
        take(b.at(0) - 4096, 64, &t);
        CHECK(t.state == Taken::kNoList);
        take(b.at(500), 64 * 4, &t);  // past the end
        CHECK(t.state == Taken::kNoList);
        const u64 unkeyed = g_unkeyed.load();
        host_lists::note(b.cb(), b.at(600), 0x47);
        CHECK(g_unkeyed.load() == unkeyed + 1);
        const Seen s0 = seen();
        take(b.at(0) - 4096, 64, &t);
        shadow_begin(&t, b.at(0) - 4096, 64);
        shadow_end();
        CHECK(seen().no_list == s0.no_list + 1);
    }

    // An indirect buffer inside a range: its walk compared with its own list.
    {
        Buffer ring(256), callee(256);
        ring.put(0, 0x3f, 4);
        ring.put(4, 0x49, 7);
        ring.note(0, 0x3f);
        ring.note(4, 0x49);
        callee.put(0, 0x10, 4, kGxDispatchMagic);
        callee.put(4, 0x47, 6);
        callee.note(0, 0x10);
        callee.note(4, 0x47);
        Taken t;
        take(ring.at(0), 11 * 4, &t);
        const Seen s0 = seen();
        const u64 nested = g.nested_covered.load();
        shadow_begin(&t, ring.at(0), 11 * 4);
        shadow_packet(ring.at(0), 0x3f, &ring.chunk[1], 3);
        shadow_push(callee.at(0), 10 * 4);
        shadow_packet(callee.at(0), 0x10, &callee.chunk[1], 3);
        shadow_packet(callee.at(4), 0x47, &callee.chunk[5], 5);
        shadow_pop();
        shadow_packet(ring.at(4), 0x49, &ring.chunk[5], 6);
        shadow_end();
        CHECK(seen().covered == s0.covered + 1 && g.nested_covered.load() == nested + 1);
        // The callee's list missing a packet: the range differs.
        callee.put(10, 0x3c, 7);
        shadow_begin(&t, ring.at(0), 11 * 4);
        shadow_packet(ring.at(0), 0x3f, &ring.chunk[1], 3);
        shadow_push(callee.at(0), 17 * 4);
        shadow_packet(callee.at(0), 0x10, &callee.chunk[1], 3);
        shadow_packet(callee.at(4), 0x47, &callee.chunk[5], 5);
        shadow_packet(callee.at(10), 0x3c, &callee.chunk[11], 6);
        shadow_pop();
        shadow_packet(ring.at(4), 0x49, &ring.chunk[5], 6);
        shadow_end();
        CHECK(seen().differed == s0.differed + 1);
    }

    // What the walk acts on, by packet.
    {
        const std::uint32_t token[] = {kGxNativeMagic, 0, 0}, marker[] = {0x68750002u}, zpass[] = {57u | (1u << 8), 0, 0},
                            flush[] = {0x2cu, 0, 0};
        CHECK(carried(0x10, token, 3) && !carried(0x10, marker, 1) && !carried(0x10, nullptr, 0));
        CHECK(carried(0x46, zpass, 3) && !carried(0x46, flush, 3));
        for (std::uint32_t op : {0x15u, 0x16u, 0x20u, 0x24u, 0x25u, 0x27u, 0x2du, 0x35u, 0x33u, 0x37u, 0x3cu, 0x3fu, 0x47u, 0x48u, 0x49u,
                                 0x50u, 0x81u, 0x83u, 0x84u, 0x85u, 0x86u, 0x88u})
            CHECK(carried(op, token, 3));
        for (std::uint32_t op : {0x11u, 0x12u, 0x13u, 0x26u, 0x28u, 0x2au, 0x2fu, 0x58u, 0x68u, 0x69u, 0x76u, 0x79u, 0x80u})
            CHECK(!carried(op, token, 3));
    }

    // Many threads making and filling chunks' lists at once - two at a time
    // on the same chunk's first packet, as a race to make its region - while
    // this thread takes their ranges.
    {
        constexpr int kThreads = 8, kChunks = 64, kPackets = 200;
        std::vector<Buffer> bufs;
        bufs.reserve(kChunks);
        for (int c = 0; c < kChunks; ++c) {
            bufs.emplace_back(1024);
            for (int k = 0; k < kPackets; ++k) bufs.back().put(static_cast<std::size_t>(k) * 4, 0x10, 4, kGxNativeMagic);
        }
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&bufs, t] {
                for (int c = t; c < kChunks; c += kThreads) {
                    for (int k = 0; k < kPackets; ++k) bufs[c].note(static_cast<std::size_t>(k) * 4, 0x10);
                }
            });
        }
        // Two threads making one new chunk's region at once: one region.
        const u64 key = reinterpret_cast<u64>(carve(4096));
        std::thread a([key] { (void)region_of(key, key + 4096 * 4); });
        std::thread b2([key] { (void)region_of(key, key + 4096 * 4); });
        Taken t;
        for (int round = 0; round < 200; ++round) take(bufs[round % kChunks].at(0), 64, &t);
        for (auto& th : threads) th.join();
        a.join();
        b2.join();
        int bad = 0;
        for (int c = 0; c < kChunks; ++c) {
            take(bufs[c].at(0), kPackets * 4 * 4, &t);
            if (t.state != Taken::kTaken || t.entries.size() != kPackets) ++bad;
            for (std::size_t k = 0; k < t.entries.size(); ++k)
                if (t.entries[k] != e(static_cast<std::uint32_t>(k) * 4, 0x10)) ++bad;
        }
        CHECK(bad == 0);
        int regions_at = 0;
        for (const Slot& s : g_slots) regions_at += s.key.load() == key;
        CHECK(regions_at == 1);
    }

    report();
    std::printf("host_lists: %s\n", g_failed ? "FAILED" : "ok");
    return g_failed ? 1 : 0;
}
