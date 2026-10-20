// The item-lot roll (docs/decomp.md): sub_1bceaf0 at Binary Ninja 0x1bceaf0,
// what an enemy, a corpse, a chest or a scripted award gives, from
// ItemLotParam. Its callers - the kill reward (0x1cc54fe), the lot-to-items
// step MapItemMan's drops and awards go through (0x1e8b080), and a debug
// menu preview (0x1bcd17c) - pass a context:
//
//   +0x00 s32  the first row: the roll reads it and every row after it until
//              one is missing
//   +0x04 f32  item discovery, for the slots that ask for it (enableLuck)
//   +0x08 u8   1: a slot whose flag is on is passed over, and the results
//              keep their flags; 0: no flag is read, and every result's flag
//              is -1
//   +0x09 u8   added to every slot's flag
//   +0x0a u8   the level a category-15 slot finds its ItemLotLvdepParam row by
//   +0x0c u32  out: how many results
//   +0x10      out: the results, 0x50 bytes each, from the game's heap
//              (0x5940420), which the callers free
//
// A row (ITEMLOT_PARAM_ST) has eight slots: item id (+0x00), category
// (+0x20), base points (u16 +0x40), cumulative points (u16 +0x50), flag
// (+0x60, 0 for the row's own at +0x80), count (u8 +0x8a), and two bits in
// the u16 at +0x92: discovery applies (bit i), the counter resets when it is
// picked (bit 8 + i). A row's counter is the 8 flags from +0x84 read as a
// number, capped at +0x88; with it at count c of max m, a slot weighs
//   base*100 + c/m * (cumulative*100 - base*100), times discovery
// truncated, and at least 1 - the "pity" that makes a rare drop likelier the
// more often it has been missed. A slot with neither points is not in the
// running; a category-15 slot becomes the item its level's ItemLotLvdepParam
// row names (none: nothing, count 0).
//
// One draw a row, from the game's MT19937 (the object at +0xa78 of the
// singleton at 0x5956678), the ordinary tempering, scaled to the row's total
// weight; the slot it lands in is picked (past the end: the last slot with a
// weight). A row with a total of 0 or less draws nothing. Every row that
// drew counts its counter up one (to 255, sub_1bd11f0, through
// SetEventFlag), after the last row, unless a picked slot reset it - those
// eight flags are cleared after the counts, byte by byte, not through
// SetEventFlag (so no flag watcher sees them, as with the game's).
//
// The picked items are merged by flag into the results, in the order of a
// key - flag ^ 0x80000000 for a flag of 0 or more, -flag below that - up to
// six items a result, the result's rarity the highest of its slots'. Every
// byte of a result the roll does not write is zero.
//
// Ours runs that from the same rows, flags and generator, and makes the same
// calls to the game for what it does not own: the rows, the generator's
// refill, the counters, the heap. The game's own trees for the merge are
// ours as a map. The debug menu's variant of the roll (a byte at 0x593d6d8,
// with the second argument 1) and the debug preview (a third argument) stay
// the game's.
#include "decomp/decomp.h"

#include "core/thunk.h"
#include "decomp/events/flag_store.h"
#include "decomp/guest.h"
#include "hle/guest_fs.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include <xmmintrin.h>

using namespace decomp;

namespace {

using i64 = std::int64_t;

constexpr u64 kRoll = 0x1bceaf0;
constexpr u64 kLotRow = 0x2320640;       // ItemLotParam's row by id: (out {s32, row*}, id)
constexpr u64 kLvdepRow = 0x23202a0;     // ItemLotLvdepParam's row by id and level
constexpr u64 kTwist = 0x1c1e5c0;        // the generator's refill
constexpr u64 kCountUp = 0x1bd11f0;      // a counter's eight flags one up, to 255
constexpr u64 kFindHeap = 0x247b720;     // the heap a block came from
constexpr u64 kDebugVariant = 0x593d6d8;  // u8, set from the debug menu
constexpr u64 kFlagManSlot = 0x593b100;  // SprjEventFlagMan
constexpr u64 kGameState = 0x5956678;    // the generator at +0xa78
constexpr u64 kHeapSlot = 0x5940420;     // the game's default heap
constexpr u64 kHintTpoff = 0x57e44a8;    // i64: a thread's heap-hint block in its TLS (the hint at +0x10)

// push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; and rsp, -32
constexpr u8 kEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                         0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff, 0xff, 0xff};

struct Context {
    s32 lot;
    float discovery;
    u8 honour_flags;
    u8 flag_offset;
    u8 level;
    u8 pad;
    u32 count;
    u64 results;
};
static_assert(sizeof(Context) == 0x18, "the callers' context, as far as the roll reads it");

struct ResultItem {
    s32 id;
    s32 category;
    u8 num;
    u8 slot;
    u16 pad;
};
struct Result {
    s32 flag;
    u8 rarity;
    u8 count;
    u16 pad;
    ResultItem items[6];
};
static_assert(sizeof(Result) == 0x50, "a result entry");

void* vfunc(u64 object, u64 slot) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(ref<u64>(ref<u64>(object) + slot))); }

struct RowOut {
    s32 index = -1;
    s32 pad = 0;
    u64 row = 0;
};

u64 lot_row(s32 id) {
    RowOut out;
    hle_call_guest(game_function<void*>(kLotRow), &out, id);
    return out.row;
}

u64 lvdep_row(s32 id, u8 level) {
    RowOut out;
    hle_call_guest(game_function<void*>(kLvdepRow), &out, id, static_cast<u32>(level));
    return out.row;
}

// The flag store as the roll reads it, inline (not through IsEventFlag):
// a flag in no block, or with no manager, is off. (With no manager the
// game's would panic.)
struct Direct {
    template <class T>
    bool operator()(u64 a, T* out) const {
        *out = ref<T>(a);
        return true;
    }
};

bool flag_on(u32 id) {
    const u64 man = ref<u64>(game_address(kFlagManSlot));
    u64 byte = 0;
    u8 mask = 0;
    return man && sprj_event_flag::locate(man, id, Direct{}, &byte, &mask) && (ref<u8>(byte) & mask);
}

void flag_clear(u32 id) {
    const u64 man = ref<u64>(game_address(kFlagManSlot));
    u64 byte = 0;
    u8 mask = 0;
    if (man && sprj_event_flag::locate(man, id, Direct{}, &byte, &mask)) ref<u8>(byte) &= static_cast<u8>(~mask);
}

// One word from the game's MT19937, tempered as the roll tempers it.
u32 draw() {
    const u64 mt = ref<u64>(game_address(kGameState)) + 0xa78;
    if (--ref<u32>(mt + 4) == 0) hle_call_guest(game_function<void*>(kTwist), mt);
    const u64 next = ref<u64>(mt + 8);
    ref<u64>(mt + 8) = next + 4;
    u32 y = ref<u32>(next);
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

// cvttss2si to 64 bits, kept to the low 32 - a float past the range comes
// out 0x8000000000000000, as the game's does.
u32 truncate_low(float v) { return static_cast<u32>(static_cast<u64>(_mm_cvttss_si64(_mm_set_ss(v)))); }

// The game's release of a heap block, inline in the roll: the thread's heap
// hint when one is set (taken, and cleared), else the heap the block came
// from. (With neither the game's would panic, "Heap not found for releasing
// buffer."; ours keeps the block.)
void release(u64 block) {
    u64 heap = 0;
    if (t_guest_fs) {
        const u64 hint = reinterpret_cast<u64>(t_guest_fs) + static_cast<u64>(ref<i64>(game_address(kHintTpoff))) + 0x10;
        heap = ref<u64>(hint);
        ref<u64>(hint) = 0;
    }
    if (!heap) heap = static_cast<u64>(hle_call_guest(game_function<void*>(kFindHeap), block));
    if (heap) hle_call_guest(vfunc(heap, 0x70), heap, block);
}

struct Slot {
    s32 id, category;
    u32 weight;
    s32 flag;
    u8 num, rarity, index;
    bool resets;
};

std::atomic<u64> g_rolls{0}, g_rows{0}, g_draws{0}, g_results{0}, g_game_variant{0};

}  // namespace

namespace item_lot {

// The roll, as ours.
GUEST_ABI void roll(u64 ctx_at, u32 mode, u64 trace) {
    auto& ctx = ref<Context>(ctx_at);
    ctx.count = 0;
    if (ctx.results) {
        release(ctx.results);
        ctx.results = 0;
    }
    std::set<s32> count_up, reset;
    std::map<s32, Result> merged;
    for (s32 id = ctx.lot;; id = static_cast<s32>(static_cast<u32>(id) + 1)) {
        const u64 row = lot_row(id);
        if (!row) break;
        g_rows.fetch_add(1, std::memory_order_relaxed);
        const s32 row_flag = ref<s32>(row + 0x80), counter = ref<s32>(row + 0x84);
        const u8 counter_max = ref<u8>(row + 0x88);
        float ratio = 0.0f;
        if (counter_max != 0 && counter >= 0) {
            u32 count = 0;
            for (u32 i = 0; i < 8; ++i) {
                const s32 f = static_cast<s32>(static_cast<u32>(counter) + i);
                if (f >= 0 && flag_on(static_cast<u32>(f))) count |= 1u << i;
            }
            if (count > counter_max) count = counter_max;
            ratio = static_cast<float>(static_cast<i64>(count)) / static_cast<float>(static_cast<s32>(counter_max));
        }
        const u16 bits = ref<u16>(row + 0x92);
        Slot slots[8];
        int n = 0;
        s32 total = 0;
        for (u32 i = 0; i < 8; ++i) {
            const float base = static_cast<float>(static_cast<s32>(ref<u16>(row + 0x40 + 2 * i) * 100u));
            const float cumulative = static_cast<float>(static_cast<s32>(ref<u16>(row + 0x50 + 2 * i) * 100u));
            if (!(base > 0.0f) && !(cumulative > 0.0f)) continue;
            s32 flag = ref<s32>(row + 0x60 + 4 * i);
            if (flag == 0) flag = row_flag;
            flag = static_cast<s32>(static_cast<u32>(flag) + ctx.flag_offset);
            if (ctx.honour_flags && flag >= 0 && flag_on(static_cast<u32>(flag))) continue;
            const float d = cumulative - base;
            const float m = ratio * d;
            float w = base + m;
            w = w * (((bits >> i) & 1) ? ctx.discovery : 1.0f);
            u32 weight = 0;
            if (w > 0.0f) {
                weight = truncate_low(w);
                if (weight <= 1) weight = 1;
            }
            Slot& s = slots[n++];
            s.id = ref<s32>(row + 4 * i);
            s.category = ref<s32>(row + 0x20 + 4 * i);
            s.weight = weight;
            s.flag = flag;
            s.num = ref<u8>(row + 0x8a + i);
            s.rarity = ref<u8>(row + 0x89);
            s.index = static_cast<u8>(i);
            s.resets = ((bits >> (8 + i)) & 1) != 0;
            if (s.category == 15) {
                if (const u64 lv = lvdep_row(s.id, ctx.level)) {
                    s.id = ref<s32>(lv + 4);
                    s.category = ref<s32>(lv);
                } else {
                    s.id = -1;
                    s.category = -1;
                    s.num = 0;
                }
            }
            total = static_cast<s32>(static_cast<u32>(total) + weight);
        }
        if (total <= 0) continue;
        count_up.insert(counter);
        g_draws.fetch_add(1, std::memory_order_relaxed);
        const u32 y = draw();
        if (n <= 0) continue;
        const float scaled = static_cast<float>(static_cast<s32>(y >> 9)) * 1.1920928955078125e-07f;
        s32 pick = static_cast<s32>(truncate_low(static_cast<float>(total) * scaled));
        int chosen = -1, last = -1;
        for (int j = 0; j < n; ++j) {
            const s32 w = static_cast<s32>(slots[j].weight);
            if (w != 0) last = j;
            if (pick < w) {
                chosen = j;
                break;
            }
            pick = static_cast<s32>(static_cast<u32>(pick) - static_cast<u32>(w));
        }
        if (chosen < 0) chosen = last;
        if (chosen < 0) continue;
        const Slot& s = slots[chosen];
        if (s.resets) reset.insert(counter);
        if (s.num == 0 || (s.category | s.id) < 0) continue;
        const s32 key = s.flag >= 0 ? static_cast<s32>(static_cast<u32>(s.flag) ^ 0x80000000u)
                                    : static_cast<s32>(0u - static_cast<u32>(s.flag));
        auto it = merged.find(key);
        if (it == merged.end()) {
            Result& r = merged.emplace(key, Result{}).first->second;
            r.flag = s.flag;
            r.rarity = s.rarity;
            r.count = 0;
            it = merged.find(key);
        } else {
            Result& r = it->second;
            if (s.rarity > r.rarity) r.rarity = s.rarity;
            if (r.count > 5) continue;
        }
        Result& r = it->second;
        r.items[r.count] = {s.id, s.category, s.num, s.index, 0};
        ++r.count;
    }
    for (const s32 c : count_up) hle_call_guest(game_function<void*>(kCountUp), c);
    for (const s32 c : reset) {
        if (c < 0) continue;
        for (u32 i = 0; i < 8; ++i) {
            const s32 f = static_cast<s32>(static_cast<u32>(c) + i);
            if (f >= 0) flag_clear(static_cast<u32>(f));
        }
    }
    const u32 n = static_cast<u32>(merged.size());
    if (n) {
        const u64 heap = ref<u64>(game_address(kHeapSlot));
        const u64 out = static_cast<u64>(hle_call_guest(vfunc(heap, 0x58), heap, static_cast<u64>(n) * sizeof(Result), 4));
        ctx.results = out;
        if (out) {
            ctx.count = n;
            u64 dst = out;
            for (const auto& [key, r] : merged) {
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)), &r, sizeof r);
                if (!ctx.honour_flags) ref<s32>(dst) = -1;
                dst += sizeof(Result);
            }
            g_results.fetch_add(n, std::memory_order_relaxed);
        }
    }
    g_rolls.fetch_add(1, std::memory_order_relaxed);
    (void)mode;
    (void)trace;
}

}  // namespace item_lot

namespace {

void* g_game_roll = nullptr;

// The debug menu's variant and preview stay the game's.
bool games_own(u32 mode, u64 trace) { return trace || (ref<u8>(game_address(kDebugVariant)) && static_cast<u8>(mode) == 1); }

GUEST_ABI void roll_entry(u64 ctx, u32 mode, u64 trace) {
    if (games_own(mode, trace)) {
        g_game_variant.fetch_add(1, std::memory_order_relaxed);
        hle_call_guest(g_game_roll, ctx, mode, trace);
        return;
    }
    item_lot::roll(ctx, mode, trace);
}

// ---- The compare ----------------------------------------------------------
//
// The game's roll runs and its results stand; ours then runs from the same
// state - the generator and the counters put back as they were - and has to
// leave the same results, the same generator and the same counters, which
// are then put back as the game's left them. The flags a roll writes are the
// counters of the rows it reads (their watchers see the game's writes and
// ours: a compare run only).

DecompCompare g_compare;
std::atomic<int> g_compare_logs{0};
constexpr u64 kGeneratorBytes = 0x9d0;

struct FlagState {
    u32 id;
    bool on;
};

std::vector<FlagState> counter_flags(s32 lot) {
    std::vector<FlagState> out;
    std::set<s32> seen;
    for (s32 id = lot;; id = static_cast<s32>(static_cast<u32>(id) + 1)) {
        const u64 row = lot_row(id);
        if (!row) break;
        const s32 c = ref<s32>(row + 0x84);
        if (c < 0 || !seen.insert(c).second) continue;
        for (u32 i = 0; i < 8; ++i) {
            const s32 f = static_cast<s32>(static_cast<u32>(c) + i);
            if (f >= 0) out.push_back({static_cast<u32>(f), flag_on(static_cast<u32>(f))});
        }
    }
    return out;
}

void put_flags(const std::vector<FlagState>& flags) {
    const u64 man = ref<u64>(game_address(kFlagManSlot));
    for (const FlagState& f : flags) {
        u64 byte = 0;
        u8 mask = 0;
        if (!man || !sprj_event_flag::locate(man, f.id, Direct{}, &byte, &mask)) continue;
        ref<u8>(byte) = f.on ? static_cast<u8>(ref<u8>(byte) | mask) : static_cast<u8>(ref<u8>(byte) & ~mask);
    }
}

std::vector<FlagState> read_flags(std::vector<FlagState> flags) {
    for (FlagState& f : flags) f.on = flag_on(f.id);
    return flags;
}

GUEST_ABI void compare_roll(u64 ctx_at, u32 mode, u64 trace) {
    if (games_own(mode, trace)) {
        hle_call_guest(g_game_roll, ctx_at, mode, trace);
        return;
    }
    auto& ctx = ref<Context>(ctx_at);
    const u64 mt = ref<u64>(game_address(kGameState)) + 0xa78;
    const std::vector<FlagState> flags_before = counter_flags(ctx.lot);
    std::vector<u8> mt_before(kGeneratorBytes);
    std::memcpy(mt_before.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(mt)), kGeneratorBytes);
    Context mine = ctx;
    mine.count = 0;
    mine.results = 0;

    hle_call_guest(g_game_roll, ctx_at, mode, trace);
    const std::vector<FlagState> flags_game = read_flags(flags_before);
    std::vector<u8> mt_game(kGeneratorBytes);
    std::memcpy(mt_game.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(mt)), kGeneratorBytes);

    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(mt)), mt_before.data(), kGeneratorBytes);
    put_flags(flags_before);
    item_lot::roll(reinterpret_cast<u64>(&mine), mode, trace);
    const std::vector<FlagState> flags_ours = read_flags(flags_before);

    const auto* theirs = reinterpret_cast<const u8*>(static_cast<std::uintptr_t>(ctx.results));
    const auto* ours = reinterpret_cast<const u8*>(static_cast<std::uintptr_t>(mine.results));
    bool same = mine.count == ctx.count && (ctx.count == 0 || (theirs && ours && std::memcmp(theirs, ours, ctx.count * sizeof(Result)) == 0));
    const bool same_mt = std::memcmp(mt_game.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(mt)), kGeneratorBytes) == 0;
    bool same_flags = true;
    for (std::size_t i = 0; i < flags_game.size(); ++i) same_flags = same_flags && flags_game[i].on == flags_ours[i].on;
    g_compare.calls.fetch_add(1, std::memory_order_relaxed);
    if (!(same && same_mt && same_flags)) {
        g_compare.differ.fetch_add(1, std::memory_order_relaxed);
        if (g_compare_logs.fetch_add(1) < 8)
            host_log("decomp: the item-lot roll from row %d differs: results %u/%u%s, generator %s, counters %s", ctx.lot, ctx.count,
                     mine.count, same ? "" : " (not the same bytes)", same_mt ? "the same" : "not the same",
                     same_flags ? "the same" : "not the same");
    }
    if (mine.results) {
        const u64 heap = ref<u64>(game_address(kHeapSlot));
        hle_call_guest(vfunc(heap, 0x70), heap, mine.results);
    }
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(mt)), mt_game.data(), kGeneratorBytes);
    put_flags(flags_game);
}

void report() {
    host_log("decomp: the item-lot roll: %llu rolls (%llu left to the game's debug variant), %llu rows, %llu draws, %llu results",
             static_cast<ull>(g_rolls.load()), static_cast<ull>(g_game_variant.load()), static_cast<ull>(g_rows.load()),
             static_cast<ull>(g_draws.load()), static_cast<ull>(g_results.load()));
}

}  // namespace

void decomp_item_lots_add() {
    decomp_add({"sub_1bceaf0", "items", kRoll, kEntry, sizeof kEntry, reinterpret_cast<void*>(&roll_entry), DecompKind::Hosted,
                &g_game_roll, reinterpret_cast<void*>(&compare_roll), &g_compare, &report});
}
