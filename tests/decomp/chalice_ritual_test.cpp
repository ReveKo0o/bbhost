// Chalice dungeons (decomp/chalice/ritual.cpp) against the game's own code
// (tests/decomp/eboot_kit.h): the roll, a rite's pick, the map uid and the feature
// pairs run in the loaded eboot and as ours on the same inputs - synthetic
// HolygrailExParam and DungeonSubFeatLotParam rows served by stubs both
// call, a flag store holding the unlock flags, a scripted generator - and
// every byte they write, every draw they make, compared. The game's roll and
// pick keep their candidates on the thread's runtime heap; the test gives
// the thread one (malloc behind the heap's vtable).
#include "decomp/chalice/ritual.cpp"

#include "eboot_kit.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <vector>



namespace {

eboot_kit::Rng g_rng(0xc4a11ce);

// ---- the params, served by stubs at the game's lookups ----------------------

std::map<s32, std::vector<u8>> g_holygrail;  // HolygrailExParam rows, 0x88 bytes
std::map<s32, std::vector<u8>> g_subfeat;    // DungeonSubFeatLotParam rows, 0x3c bytes

void holygrail_row(Setup* out, s32 id) {
    const auto it = g_holygrail.find(id);
    out->id = id;
    out->row = it == g_holygrail.end() ? nullptr : it->second.data();
}

void subfeat_row(Row* out, s32 id) {
    const auto it = g_subfeat.find(id);
    out->id = id;
    out->row = it == g_subfeat.end() ? nullptr : it->second.data();
}

// ---- a generator with a script ----------------------------------------------

struct ScriptedRng {
    void* const* vtable;
    std::uint64_t state;
    int draws;
};

u32 rng_next(void* self) {
    auto* r = static_cast<ScriptedRng*>(self);
    ++r->draws;
    r->state ^= r->state >> 12;  // xorshift64*
    r->state ^= r->state << 25;
    r->state ^= r->state >> 27;
    return static_cast<u32>((r->state * 0x2545f4914f6cdd1dull) >> 32);
}

void* const kRngVtable[4] = {nullptr, nullptr, reinterpret_cast<void*>(&rng_next), nullptr};

// ---- the thread's runtime heap, as the game's containers find it -------------

u8* heap_caps(u8* out, void*, int) {
    std::memset(out, 0, 8);
    out[0] = 0x20;  // a heap containers may use
    return out;
}
void* heap_alloc(void*, std::size_t size, std::size_t) { return std::malloc(size ? size : 1); }
void heap_free(void*, void* p) { std::free(p); }

void* g_heap_vtable[16];
alignas(16) std::uint8_t g_runtime[0x100];

void give_thread_a_heap() {
    g_heap_vtable[0x20 / 8] = reinterpret_cast<void*>(&heap_caps);
    g_heap_vtable[0x58 / 8] = reinterpret_cast<void*>(&heap_alloc);
    g_heap_vtable[0x70 / 8] = reinterpret_cast<void*>(&heap_free);
    void* const vt = g_heap_vtable;
    std::memcpy(g_runtime + 0x28, &vt, 8);
    std::uint8_t* const runtime = g_runtime;
    std::memcpy(static_cast<std::uint8_t*>(eboot_kit::tcb()) - 0x730, &runtime, 8);  // rel_dlruntime_obj_mb
}

// ---- the flag store (decomp/events/flag_store.h's layout) ------------------

struct FlagStore {
    alignas(16) std::uint8_t man[0x40] = {};
    std::vector<std::array<std::uint8_t, 0x40>> nodes;  // [0] is the head
    std::vector<std::vector<std::uint8_t>> blocks;
    std::vector<std::uint8_t> pool;
    u32 block_size = 1000;

    std::uint64_t node_at(std::size_t i) { return reinterpret_cast<std::uint64_t>(nodes[i].data()); }

    // A balanced tree over keys[lo, hi), its root's index (0: none).
    std::size_t build(const std::vector<u32>& keys, std::size_t lo, std::size_t hi, std::size_t first) {
        if (lo >= hi) return 0;
        const std::size_t mid = (lo + hi) / 2, i = first + mid;
        const std::size_t left = build(keys, lo, mid, first), right = build(keys, mid + 1, hi, first);
        const std::uint64_t head = node_at(0), l = left ? node_at(left) : head, r = right ? node_at(right) : head;
        std::memcpy(nodes[i].data() + 0x00, &l, 8);
        std::memcpy(nodes[i].data() + 0x10, &r, 8);
        nodes[i][0x19] = 0;
        std::memcpy(nodes[i].data() + 0x20, &keys[mid], 4);
        return i;
    }

    void make(const std::vector<u32>& keys) {
        nodes.assign(keys.size() + 1, {});
        blocks.assign(keys.size(), std::vector<std::uint8_t>(block_size / 8 + 1));
        pool.assign((keys.size() + 1) * (block_size / 8 + 1), 0);
        nodes[0][0x19] = 1;  // the head is the nil node
        const std::size_t root = build(keys, 0, keys.size(), 1);
        const std::uint64_t r = root ? node_at(root) : node_at(0);
        std::memcpy(nodes[0].data() + 8, &r, 8);
        const u32 stride = block_size / 8 + 1;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            std::uint8_t* n = nodes[1 + k].data();
            for (auto& b : blocks[k]) b = static_cast<std::uint8_t>(g_rng.u32());
            const u32 kind = g_rng.chance(0.1) ? 7 : g_rng.chance(0.5) ? 2 : 1;  // 7: a kind the game does not read
            std::memcpy(n + 0x28, &kind, 4);
            if (kind == 1) {
                const u32 index = static_cast<u32>(k + 1);
                std::memcpy(n + 0x30, &index, 4);
                std::memcpy(pool.data() + index * stride, blocks[k].data(), blocks[k].size());
            } else {
                const std::uint64_t data = g_rng.chance(0.05) ? 0 : reinterpret_cast<std::uint64_t>(blocks[k].data());
                std::memcpy(n + 0x30, &data, 8);
            }
        }
        std::memcpy(man + 0x1c, &block_size, 4);
        std::memcpy(man + 0x20, &stride, 4);
        const std::uint64_t p = reinterpret_cast<std::uint64_t>(pool.data()), h = node_at(0);
        std::memcpy(man + 0x28, &p, 8);
        std::memcpy(man + 0x38, &h, 8);
        const std::uint64_t self = reinterpret_cast<std::uint64_t>(man);
        std::memcpy(eboot_kit::at(kEventFlagManSlot), &self, 8);
    }
};

FlagStore g_flags;

// ---- generated params --------------------------------------------------------

float rate() {
    static const float kOdd[] = {0.0f, -0.0f, -1.0f, 1e-40f, std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::quiet_NaN(), 1e30f};
    if (g_rng.chance(0.3)) return 0.0f;
    if (g_rng.chance(0.1)) return kOdd[g_rng.range(0, static_cast<int>(std::size(kOdd)) - 1)];
    return g_rng.uniform(0.0f, 100.0f);
}

std::vector<s32> g_lot_ids;

s32 a_lot() {
    if (g_rng.chance(0.1)) return -1;
    if (g_rng.chance(0.05)) return kAutoSelectedLot;
    if (g_rng.chance(0.05)) return static_cast<s32>(g_rng.u32() & 0x7fffffff);  // no such row
    return g_lot_ids[static_cast<std::size_t>(g_rng.range(0, static_cast<int>(g_lot_ids.size()) - 1))];
}

void make_params(u32 flag_base) {
    g_subfeat.clear();
    g_lot_ids.clear();
    for (int i = 0; i < 40; ++i) {
        const s32 id = i == 0 ? kAutoSelectedLot : 20000 + i * 10;
        std::vector<u8> row(0x3c);
        for (int k = 0; k < 10; ++k) row[kSubFeature + k] = static_cast<u8>(g_rng.u32());
        for (int k = 0; k < 10; ++k) {
            const float r = rate();
            std::memcpy(row.data() + kRate + 4 * k, &r, 4);
        }
        if (g_rng.chance(0.9)) g_subfeat[id] = std::move(row);
        g_lot_ids.push_back(id);
    }
    g_holygrail.clear();
    for (int i = 0; i < 20; ++i) {
        std::vector<u8> row(0x88);
        for (auto& b : row) b = static_cast<u8>(g_rng.u32());
        static const u8 kLayouts[] = {0, 1, 2, 100, 200, 255};
        row[kVariations] = g_rng.chance(0.8) ? kLayouts[g_rng.range(0, 5)] : static_cast<u8>(g_rng.u32());
        const s32 base = g_rng.chance(0.85) ? static_cast<s32>(flag_base + static_cast<u32>(g_rng.range(0, 3000)))
                                            : g_rng.chance(0.5) ? -g_rng.range(0, 20) : static_cast<s32>(g_rng.u32());
        std::memcpy(row.data() + kUniqueBaseFlag, &base, 4);
        for (int k = 0; k < 15; ++k) {
            if (g_rng.chance(0.3)) row[kRangeHead[k]] = 0xff;
            if (g_rng.chance(0.3)) row[kRangeTail[k]] = 0xff;
            else if (g_rng.chance(0.6)) row[kRangeTail[k]] = static_cast<u8>(row[kRangeHead[k]] + g_rng.range(0, 40));
        }
        const s32 direct = g_rng.chance(0.7) ? -1 : g_rng.chance(0.5) ? 29109000 + g_rng.range(0, 99) : static_cast<s32>(g_rng.u32());
        std::memcpy(row.data() + kDirectMapUid, &direct, 4);
        for (u8 at : kFixedLot) { const s32 l = a_lot(); std::memcpy(row.data() + at, &l, 4); }
        for (u8 at : kFreeLot) { const s32 l = a_lot(); std::memcpy(row.data() + at, &l, 4); }
        for (u8 at : kGroupLot) { const s32 l = a_lot(); std::memcpy(row.data() + at, &l, 4); }
        g_holygrail[6000 + i] = std::move(row);
    }
}

int g_bad = 0;

void report(const char* what, int c, const void* a, const void* b, std::size_t n) {
    if (++g_bad > 10) return;
    std::printf("%s: case %d differs:\n  game:", what, c);
    for (std::size_t i = 0; i < n; ++i) std::printf(" %02x", static_cast<const u8*>(a)[i]);
    std::printf("\n  ours:");
    for (std::size_t i = 0; i < n; ++i) std::printf(" %02x", static_cast<const u8*>(b)[i]);
    std::printf("\n");
}

}  // namespace

int main() {
    eboot_kit::load("decomp_chalice_ritual");
    const std::uint8_t* code = eboot_kit::at(kRoll);
    static const std::uint8_t kExpect[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
    if (std::memcmp(code, kExpect, sizeof kExpect) != 0) {
        std::printf("decomp_chalice_ritual: the roll is not where the decomp expects it\n");
        return 1;
    }
    eboot_kit::stub(kHolygrailRow, reinterpret_cast<const void*>(&holygrail_row));
    eboot_kit::stub(kSubFeatLotRow, reinterpret_cast<const void*>(&subfeat_row));
    give_thread_a_heap();

    const auto game_roll = eboot_kit::fn<void(GUEST_ABI*)(Setup*, s32, void*)>(kRoll);
    const auto game_pick = eboot_kit::fn<s32(GUEST_ABI*)(const Row*, void*)>(kPick);
    const auto game_uid = eboot_kit::fn<u32*(GUEST_ABI*)(u32*, const Setup*, u32)>(kMapUid);
    const auto game_pairs = eboot_kit::fn<Pair*(GUEST_ABI*)(Pair*, const Setup*)>(kPairs);

    int rolls = 0, picks = 0, uids = 0, pairs = 0, draws = 0;
    for (int batch = 0; batch < 100; ++batch) {
        g_flags.block_size = batch % 3 == 0 ? 1000 : batch % 3 == 1 ? 256 : 10000;
        const u32 flag_base = static_cast<u32>(g_rng.range(1, 200000));
        std::vector<u32> keys;
        for (u32 b = (flag_base / g_flags.block_size) ; keys.size() < 8; ++b)
            if (g_rng.chance(0.8)) keys.push_back(b);
        g_flags.make(keys);
        make_params(flag_base);

        for (int c = 0; c < 200; ++c) {
            // The roll, from the same leftovers and the same script.
            Setup a, b;
            // Leftovers of an earlier roll (what a roll without a row keeps).
            for (std::size_t i = 0; i < sizeof a; ++i) reinterpret_cast<u8*>(&a)[i] = static_cast<u8>(g_rng.u32());
            for (Choice& ch : a.choices) {
                if (g_rng.chance(0.7)) ch.lot = a_lot();
                if (g_rng.chance(0.7)) ch.selected = g_rng.chance(0.5);
            }
            std::memcpy(&b, &a, sizeof a);
            const s32 id = g_rng.chance(0.8) ? 6000 + g_rng.range(0, 19) : static_cast<s32>(g_rng.u32());
            const std::uint64_t seed = g_rng.next() | 1;
            ScriptedRng ra{kRngVtable, seed, 0}, rb{kRngVtable, seed, 0};
            game_roll(&a, id, &ra);
            chalice_roll(&b, id, &rb);
            if (std::memcmp(&a, &b, sizeof a) != 0 || ra.draws != rb.draws || ra.state != rb.state)
                report("roll (setup bytes)", c, &a, &b, sizeof a);
            ++rolls;
            draws += ra.draws;

            // The uid and the pairs of what it rolled, and of leftovers.
            for (int v = 0; v < 3; ++v) {
                const u32 variation = v == 0 ? a.variation : v == 1 ? g_rng.u32() : static_cast<u32>(g_rng.range(0, 300));
                u32 ua = 0x12345678, ub = 0x12345678;
                if (game_uid(&ua, &a, variation) != &ua || chalice_map_uid(&ub, &a, variation) != &ub || ua != ub)
                    report("map uid", c, &ua, &ub, 4);
                ++uids;
            }
            Setup junk;
            for (std::size_t i = 0; i < sizeof junk; ++i) reinterpret_cast<u8*>(&junk)[i] = static_cast<u8>(g_rng.u32());
            junk.row = g_rng.chance(0.5) ? a.row : nullptr;
            for (Choice& ch : junk.choices) {
                if (g_rng.chance(0.5)) ch.lot = a_lot();
                ch.available = g_rng.chance(0.7);
                ch.selected = g_rng.chance(0.6);
                if (g_rng.chance(0.8)) ch.pick = static_cast<u8>(g_rng.range(0, 11));
            }
            for (const Setup* s : {&a, &junk}) {
                Pair pa[9], pb[9];
                std::memset(pa, 0x5a, sizeof pa);
                std::memset(pb, 0x5a, sizeof pb);
                if (game_pairs(pa, s) != pa || chalice_feature_pairs(pb, s) != pb || std::memcmp(pa, pb, sizeof pa) != 0)
                    report("feature pairs", c, pa, pb, sizeof pa);
                ++pairs;
            }

            // A pick on its own.
            Row lot{a_lot(), nullptr};
            const auto it = g_subfeat.find(lot.id);
            if (it != g_subfeat.end() && g_rng.chance(0.95)) lot.row = it->second.data();
            const std::uint64_t pseed = g_rng.next() | 1;
            ScriptedRng pa{kRngVtable, pseed, 0}, pb{kRngVtable, pseed, 0};
            const s32 ka = game_pick(&lot, &pa), kb = chalice_rite_pick(&lot, &pb);
            if (ka != kb || pa.draws != pb.draws) report("rite pick", c, &ka, &kb, 4);
            ++picks;
        }
    }
    std::printf("decomp_chalice_ritual: %d rolls (%d draws), %d picks, %d map uids, %d pair sets; %d differ\n", rolls, draws, picks,
                uids, pairs, g_bad);
    return g_bad ? 1 : 0;
}
