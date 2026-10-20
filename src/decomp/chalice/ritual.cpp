// Chalice dungeons as our source (docs/decomp.md). There is no layout
// generator: every root dungeon is one of 100 or 200 layouts built ahead of
// time (map/mapstudio/m29_AA_BB_00/m29_AA_BB_CC.msb), and a chalice ritual
// rolls which one, and its rites, once - when the altar's ritual menu opens.
// The roll writes the menu's setup; confirming it turns the setup into a map
// uid and nine (lot, feature) pairs, which the altar keeps in event flags and
// the dungeon's load reads back. Four functions, all leaves:
//
//   dungeon_ritual_config_initialize (0x1eaeec0)  the roll
//   sub_2316e70                                   a rite's weighted pick
//   sub_231fbc0                                   setup -> map uid
//   sub_1eaf9d0                                   setup -> the nine pairs
//
// The setup (0x80 bytes; the menu's this+0x11f0):
//   +0     HolygrailExParam id (the chalice's goods id)
//   +4     untouched by all four
//   +8     the row (0x88 bytes), null when the id has none
//   +0x10  variation: which of the row's layouts
//   +0x14  13 choices of 8 bytes - available, selected, how many of the lot's
//          ten rates are positive, the picked rate, the DungeonSubFeatLotParam
//          id - the row's five fixed lots, three free ones and five groups.
//
// The random draws come from the game's own generator, through its vtable
// (+0x10, a u32): the SFMT at GameStateMan+0x70, seeded from the clock at
// boot, so no seed reproduces a roll; the results are what the altar keeps.
// A roll draws once for the variation; twice more when the row has unlock
// ranges still open (a range is open while its flag uniqueBaseFlagId + i is
// clear, its layouts head..tail weighted by their count); and once for each
// lot that has a positive rate.
//
// The game's roll and pick gather their candidates in vectors on the
// thread's runtime heap; ours keep them in fixed arrays (15 ranges, 10
// rates), the same order, the same float sums.
#include "decomp/decomp.h"
#include "decomp/events/flag_store.h"
#include "decomp/guest.h"

#include "log.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace decomp;

namespace {

constexpr u64 kRoll = 0x1eaeec0, kPick = 0x2316e70, kMapUid = 0x231fbc0, kPairs = 0x1eaf9d0;
constexpr u64 kHolygrailRow = 0x231f720;   // writes {id, row} for a HolygrailExParam id
constexpr u64 kSubFeatLotRow = 0x2316c70;  // writes {id, row} for a DungeonSubFeatLotParam id
constexpr u64 kEventFlagManSlot = 0x593b100;
constexpr u64 kDlPanic = 0x24b55b0;
// DL_PANIC's arguments when the flag manager is missing: the singleton
// header's path, its line, the message format and the class name.
constexpr u64 kSingletonFile = 0x4d3b369, kSingletonFormat = 0x4d3b3bd, kSingletonName = 0x4d3b19a;
constexpr int kSingletonLine = 0xb1;
constexpr s32 kAutoSelectedLot = 994500000;  // a rite the roll always selects (feature 45)

// HolygrailExParam: the unlock ranges' first and last layout (u8; 0xff none),
// as the game's tables at 0x4b359c0 and 0x4b35a40 give their offsets; the
// lots, fixed (0x4b35940), free and group (0x4b35990).
constexpr u8 kRangeHead[15] = {0x10, 0x12, 0x14, 0x16, 0x24, 0x26, 0x28, 0x2a, 0x2c, 0x2e, 0x30, 0x32, 0x34, 0x36, 0x58};
constexpr u8 kRangeTail[15] = {0x11, 0x13, 0x15, 0x17, 0x25, 0x27, 0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x33, 0x35, 0x37, 0x59};
constexpr u8 kFixedLot[5] = {0x38, 0x3c, 0x40, 0x44, 0x48};
constexpr u8 kFreeLot[3] = {0x4c, 0x50, 0x54};
constexpr u8 kGroupLot[5] = {0x74, 0x78, 0x7c, 0x80, 0x84};
constexpr u64 kUniqueBaseFlag = 0xc, kDirectMapUid = 0x18, kMapBase = 0x2, kVariations = 0x5;
// DungeonSubFeatLotParam: ten rates (f32, 0x4b35540) and the ten features
// they pick (s8, 0x4b354f0).
constexpr u64 kRate = 0x14, kSubFeature = 0x8;

struct Choice {
    u8 available, selected, count, pick;
    s32 lot;
};

struct Setup {
    s32 id;
    u32 untouched;
    const u8* row;
    u32 variation;
    Choice choices[13];  // fixed 0-4, free 5-7, group 8-12
};
static_assert(sizeof(Setup) == 0x80 && offsetof(Setup, variation) == 0x10 && offsetof(Setup, choices) == 0x14, "the setup");

struct Row {  // what the param lookups write
    s32 id;
    const u8* row;
};

struct Pair {
    s32 lot, feature;
};

u32 draw(void* rng) {
    const auto* vtable = *static_cast<void* const* const*>(rng);
    return reinterpret_cast<u32(GUEST_ABI*)(void*)>(vtable[2])(rng);
}

// A draw as a point in [0, 1) - its top 23 bits as a float's mantissa -
// scaled to the candidates' total weight.
float point(u32 r, float total) { return (std::bit_cast<float>((r >> 9) | 0x3f800000u) + -1.0f) * total; }

// The flag store read the roll makes inline (decomp/events/flag_store.h).
// Where the game would divide by a zero block size, or read through a
// manager DL_PANIC let it go on without, ours reads the flag as clear.
bool flag_set(s32 id) {
    u64 man = load<u64>(game_address(kEventFlagManSlot));
    if (!man) {
        game_function<void(GUEST_ABI*)(const char*, int, const char*, ...)>(kDlPanic)(
            reinterpret_cast<const char*>(game_address(kSingletonFile)), kSingletonLine,
            reinterpret_cast<const char*>(game_address(kSingletonFormat)),
            reinterpret_cast<const char*>(game_address(kSingletonName)));
        man = load<u64>(game_address(kEventFlagManSlot));
        if (!man) return false;
    }
    const auto plain = [](u64 at, auto* v) {
        std::memcpy(v, reinterpret_cast<const void*>(at), sizeof *v);
        return true;
    };
    u64 byte = 0;
    u8 mask = 0;
    return sprj_event_flag::locate(man, static_cast<u32>(id), plain, &byte, &mask) &&
           (*reinterpret_cast<const u8*>(byte) & mask) != 0;
}

}  // namespace

// sub_2316e70: one of a DungeonSubFeatLotParam row's ten rates, picked by
// weight; -1 when none is positive (or the row is missing).
DECOMP_LEAF s32 chalice_rite_pick(const Row* lot, void* rng) {
    float rate[10];
    s32 index[10];
    int n = 0;
    for (int k = 0; k < 10; ++k) {
        if (!lot->row) continue;
        const float r = load<float>(lot->row, kRate + 4 * k);
        if (!(r > 0.0f)) continue;  // NaN too
        rate[n] = r;
        index[n++] = k;
    }
    if (!n) return -1;
    float total = 0.0f;
    for (int i = 0; i < n; ++i) total += rate[i];
    const float at = point(draw(rng), total);
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        sum += rate[i];
        if (sum > at) return index[i];
    }
    return -1;
}

namespace {

// One lot of the row: its rates counted, one picked; nothing when none is
// positive or the pick fails.
void roll_choice(Choice* c, s32 lot, void* rng, u8 selected) {
    Row r{-1, nullptr};
    game_function<void(GUEST_ABI*)(Row*, s32)>(kSubFeatLotRow)(&r, lot);
    u32 count = 0;
    if (r.row)
        for (int k = 0; k < 10; ++k) count += load<float>(r.row, kRate + 4 * k) > 0.0f;
    if (!count) return;
    const s32 pick = chalice_rite_pick(&r, rng);
    if (pick < 0) return;
    c->available = 1;
    c->selected = selected;
    c->count = static_cast<u8>(count);
    c->pick = static_cast<u8>(pick);
    c->lot = lot;
}

}  // namespace

namespace {
std::atomic<u64> g_rolls{0};
}  // namespace

// dungeon_ritual_config_initialize: the roll.
DECOMP_LEAF void chalice_roll(Setup* s, s32 id, void* rng) {
    g_rolls.fetch_add(1, std::memory_order_relaxed);
    game_function<void(GUEST_ABI*)(Setup*, s32)>(kHolygrailRow)(s, id);
    s->variation = 0;
    const u32 first = draw(rng);
    u32 layouts = 1;
    if (s->row && s->row[kVariations] > 1) layouts = s->row[kVariations];
    s->variation = first % layouts;

    // The unlock ranges still open, weighted by their layouts.
    float weight[15];
    s32 range[15];
    int n = 0;
    for (u32 i = 0; i < 15; ++i) {
        if (!s->row) continue;
        const s32 flag = static_cast<s32>(load<u32>(s->row, kUniqueBaseFlag) + i);
        if (flag <= 0 || flag_set(flag)) continue;
        const u32 head = s->row[kRangeHead[i]];
        if (head == 0xff) continue;
        const s32 tail = s->row[kRangeTail[i]] == 0xff ? -1 : s->row[kRangeTail[i]];
        const s32 span = tail - static_cast<s32>(head);
        if (span < 0) continue;
        weight[n] = static_cast<float>(span + 1);
        range[n++] = static_cast<s32>(i);
    }
    if (n) {
        float total = 0.0f;
        for (int k = 0; k < n; ++k) total += weight[k];
        const float at = point(draw(rng), total);
        float sum = 0.0f;
        for (int k = 0; k < n; ++k) {
            sum += weight[k];
            if (!(sum > at)) continue;
            const u32 i = static_cast<u32>(range[k]);
            s32 head = -1, tail = -1;
            if (s->row && i <= 14) {
                head = s->row[kRangeHead[i]] == 0xff ? -1 : s->row[kRangeHead[i]];
                tail = s->row[kRangeTail[i]] == 0xff ? -1 : s->row[kRangeTail[i]];
            }
            const u32 count = static_cast<u32>(tail - head + 1);
            if (count) head = static_cast<s32>(static_cast<u32>(head) + draw(rng) % count);
            s->variation = static_cast<u32>(head);
            break;
        }
    }

    // The lots: fixed ones selected, free and group ones offered. Without a
    // row the choices are left as they were.
    if (const u8* row = s->row) {
        std::memset(s->choices, 0, sizeof s->choices);
        for (int k = 0; k < 5; ++k)
            if (const s32 lot = load<s32>(row, kFixedLot[k]); lot >= 0) roll_choice(&s->choices[k], lot, rng, 1);
        for (int k = 0; k < 3; ++k)
            if (const s32 lot = load<s32>(row, kFreeLot[k]); lot >= 0) roll_choice(&s->choices[5 + k], lot, rng, 0);
        for (int k = 0; k < 5; ++k)
            if (const s32 lot = load<s32>(row, kGroupLot[k]); lot >= 0) roll_choice(&s->choices[8 + k], lot, rng, 0);
    }
    // The rite every ritual takes: selected wherever it is offered (a group
    // one replacing the group's pick) - over stale choices too.
    for (int k = 5; k < 13; ++k) {
        if (s->choices[k].lot != kAutoSelectedLot) continue;
        if (k >= 8)
            for (int g = 8; g < 13; ++g) s->choices[g].selected = 0;
        s->choices[k].selected = 1;
    }
}

namespace {

// A uid's four bytes are its decimal digit pairs: m29_10_00_05 is 29100005.
u32 digit_pairs(u32 v) { return (v / 1000000 % 100) << 24 | (v / 10000 % 100) << 16 | (v / 100 % 100) << 8 | v % 100; }

}  // namespace

// sub_231fbc0: the setup's map uid - the row's own (directMapUid) for a fixed
// chalice, else 29,000,000 + mapBaseId * 1000 + the variation (at most the
// row's last). -1 without a row.
DECOMP_LEAF u32* chalice_map_uid(u32* out, const Setup* s, u32 variation) {
    *out = 0xffffffffu;
    if (!s->row) return out;
    const s32 direct = load<s32>(s->row, kDirectMapUid);
    if (direct >= 0) {
        *out = digit_pairs(static_cast<u32>(direct));
        return out;
    }
    const u32 layouts = s->row[kVariations];
    u32 last = layouts > 1 ? layouts - 1 : 0;
    if (last >= variation) last = variation;
    *out = digit_pairs(load<std::uint16_t>(s->row, kMapBase) * 1000u + last + 29000000u);
    return out;
}

namespace {

void pair_of(const Choice& c, Pair* p) {
    p->lot = c.lot;
    Row r{-1, nullptr};
    game_function<void(GUEST_ABI*)(Row*, s32)>(kSubFeatLotRow)(&r, c.lot);
    p->feature = r.row && c.pick <= 9 ? static_cast<std::int8_t>(r.row[kSubFeature + c.pick]) : -1;
}

}  // namespace

// sub_1eaf9d0: the nine (lot, feature) pairs the dungeon loads with - each
// selected fixed and free choice, then the first selected group; -1 for the
// rest, and for a feature whose lot has no row.
DECOMP_LEAF Pair* chalice_feature_pairs(Pair* out, const Setup* s) {
    std::memset(out, 0xff, 9 * sizeof(Pair));
    for (int k = 0; k < 8; ++k)
        if (s->choices[k].available && s->choices[k].selected) pair_of(s->choices[k], &out[k]);
    for (int k = 8; k < 13; ++k)
        if (s->choices[k].available && s->choices[k].selected) {
            pair_of(s->choices[k], &out[8]);
            break;
        }
    return out;
}

// ---- compare runs (BBHOST_DECOMP_COMPARE=1) ---------------------------------
//
// The game's roll draws from the generator it is given: the compare clones
// it - the SFMT (vtable 0x57267a0) is 0x9e0 bytes whose +0x9d8 points into
// its own state, rebased in the clone - so the game's version and ours draw
// the same numbers, and the two generators must end equal too. A generator of
// another kind is not compared (the game's runs alone). The pick, which the
// roll calls, is ours in a compare run: its own compare would draw twice.

namespace {

constexpr u64 kSfmtVtable = 0x57267a0;
constexpr std::size_t kSfmtSize = 0x9e0, kSfmtOut = 0x9d8;

void* g_roll_game = nullptr;
void* g_uid_game = nullptr;
void* g_pairs_game = nullptr;
DecompCompare g_roll_counts, g_uid_counts, g_pairs_counts;
std::atomic<u64> g_roll_unclonable{0};

// A copy of the generator at `from`, its pointer into its own state moved to
// the same place in `to`.
void rebase(u8* copy, const void* from, const void* to) {
    u64 out;
    std::memcpy(&out, copy + kSfmtOut, 8);
    out = out - reinterpret_cast<u64>(from) + reinterpret_cast<u64>(to);
    std::memcpy(copy + kSfmtOut, &out, 8);
}

GUEST_ABI void roll_compare(Setup* s, s32 id, void* rng) {
    const auto game = reinterpret_cast<void(GUEST_ABI*)(Setup*, s32, void*)>(g_roll_game);
    u64 vtable;
    std::memcpy(&vtable, rng, 8);
    if (vtable != game_address(kSfmtVtable)) {
        g_roll_unclonable.fetch_add(1, std::memory_order_relaxed);
        game(s, id, rng);
        return;
    }
    Setup ours;
    std::memcpy(&ours, s, sizeof ours);
    alignas(16) u8 clone[kSfmtSize], after[kSfmtSize];
    std::memcpy(clone, rng, kSfmtSize);
    rebase(clone, rng, clone);
    game(s, id, rng);
    chalice_roll(&ours, id, clone);
    std::memcpy(after, rng, kSfmtSize);
    rebase(after, rng, clone);  // compared with the clone, so pointing where the clone's does
    g_roll_counts.calls.fetch_add(1, std::memory_order_relaxed);
    if (std::memcmp(&ours, s, sizeof ours) != 0 || std::memcmp(after, clone, kSfmtSize) != 0) {
        if (g_roll_counts.differ.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::size_t at = 0;
            const bool setup = std::memcmp(&ours, s, sizeof ours) != 0;
            const u8* a = setup ? reinterpret_cast<const u8*>(s) : after;
            const u8* b = setup ? reinterpret_cast<const u8*>(&ours) : clone;
            while (a[at] == b[at]) ++at;
            u64 out = 0, idx = 0;
            std::memcpy(&out, static_cast<const u8*>(rng) + kSfmtOut, 8);
            std::memcpy(&idx, static_cast<const u8*>(rng) + 0x9d0, 8);
            host_log("decomp: chalice roll of %d differs in the %s at +0x%zx (%02x against ours %02x); the generator's +0x9d0 "
                     "%llx, +0x9d8 at +0x%llx of it",
                     id, setup ? "setup" : "generator", at, a[at], b[at], static_cast<unsigned long long>(idx),
                     static_cast<unsigned long long>(out - reinterpret_cast<u64>(rng)));
        }
    }
}

GUEST_ABI u32* uid_compare(u32* out, const Setup* s, u32 variation) {
    u32 ours = 0;
    chalice_map_uid(&ours, s, variation);
    u32* r = reinterpret_cast<u32*(GUEST_ABI*)(u32*, const Setup*, u32)>(g_uid_game)(out, s, variation);
    g_uid_counts.calls.fetch_add(1, std::memory_order_relaxed);
    if (r != out || *out != ours) g_uid_counts.differ.fetch_add(1, std::memory_order_relaxed);
    return r;
}

GUEST_ABI Pair* pairs_compare(Pair* out, const Setup* s) {
    Pair ours[9];
    chalice_feature_pairs(ours, s);
    Pair* r = reinterpret_cast<Pair*(GUEST_ABI*)(Pair*, const Setup*)>(g_pairs_game)(out, s);
    g_pairs_counts.calls.fetch_add(1, std::memory_order_relaxed);
    if (r != out || std::memcmp(out, ours, sizeof ours) != 0) g_pairs_counts.differ.fetch_add(1, std::memory_order_relaxed);
    return r;
}

void report() {
    const u64 n = g_rolls.load(), other = g_roll_unclonable.load();
    if (n || other)
        host_log("decomp: chalice rolls %llu as ours%s", static_cast<unsigned long long>(n),
                 other ? " (and some of a generator a compare cannot clone)" : "");
}

}  // namespace

void decomp_chalice_ritual_add() {
    static const std::uint8_t kRollEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                              0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00};
    static const std::uint8_t kPickEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                              0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
    static const std::uint8_t kPairsEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                               0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
    // mov dword [rdi], -1 / mov rcx, [rsi+8] / test rcx, rcx / je: the
    // trampoline re-aims the je.
    static const std::uint8_t kMapUidEntry[] = {0xc7, 0x07, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x4e, 0x08,
                                                0x48, 0x85, 0xc9, 0x0f, 0x84, 0x5c, 0x01, 0x00, 0x00};
    decomp_add({"dungeon_ritual_config_initialize", "chalice", kRoll, kRollEntry, sizeof kRollEntry,
                reinterpret_cast<void*>(&chalice_roll), DecompKind::Leaf, &g_roll_game,
                reinterpret_cast<void*>(&roll_compare), &g_roll_counts, &report});
    decomp_add({"sub_2316e70", "chalice", kPick, kPickEntry, sizeof kPickEntry,
                reinterpret_cast<void*>(&chalice_rite_pick), DecompKind::Leaf});
    decomp_add({"sub_231fbc0", "chalice", kMapUid, kMapUidEntry, sizeof kMapUidEntry,
                reinterpret_cast<void*>(&chalice_map_uid), DecompKind::Leaf, &g_uid_game,
                reinterpret_cast<void*>(&uid_compare), &g_uid_counts});
    decomp_add({"sub_1eaf9d0", "chalice", kPairs, kPairsEntry, sizeof kPairsEntry,
                reinterpret_cast<void*>(&chalice_feature_pairs), DecompKind::Leaf, &g_pairs_game,
                reinterpret_cast<void*>(&pairs_compare), &g_pairs_counts});
}
