// The player-data decomps (decomp/player/player_data.cpp) against the game's own
// code (tests/decomp/eboot_kit.h): item discovery, the echo gain, the kill reward,
// the death penalty and the level-up price check run in the loaded eboot and
// as ours on the same generated state - effect lists, a player and its
// vtable, the record, GameDataMan, the HUD's counters - and every byte either
// could write is compared, with what each returns and every call it makes
// (the player's virtuals, the kill's hand-on and the param lookup are stubs
// here, the same for both). Skips without the 1.09 eboot.
#include "decomp/player/player_data.cpp"

#include "eboot_kit.h"

#include <cstdio>
#include <vector>

#include <sys/mman.h>


namespace {

eboot_kit::Rng g_rng(0x1981830);

// ---- Generated state, in one arena compared byte for byte ------------------

alignas(64) std::uint8_t g_arena[1 << 16];
std::size_t g_used = 0;

u64 alloc(std::size_t n) {
    const u64 p = reinterpret_cast<u64>(g_arena + g_used);
    g_used = (g_used + n + 15) & ~std::size_t{15};
    if (g_used > sizeof g_arena) std::abort();
    return p;
}

void reset() {
    std::memset(g_arena, 0, g_used);
    g_used = 0;
}

s32 any_int() {
    switch (g_rng.range(0, 9)) {
    case 0: return 0;
    case 1: return static_cast<s32>(g_rng.u32());
    case 2: return g_rng.range(-3, 3);
    case 3: return kEchoCap - g_rng.range(-3, 3);
    case 4: return g_rng.chance(0.5) ? INT32_MAX - g_rng.range(0, 3) : INT32_MIN + g_rng.range(0, 3);
    case 5: return g_rng.range(0, 1000000000);
    default: return g_rng.range(0, 200000);
    }
}

u32 any_flag() {
    // The low byte is what is tested; the rest is noise.
    return (g_rng.u32() & 0xffffff00u) | (g_rng.chance(0.5) ? 0u : (g_rng.chance(0.8) ? 1u : g_rng.u32() & 0xff));
}

// An effects holder with 0-6 nodes; `kinds` are the stateInfos to mix in.
u64 effects() {
    const u64 holder = alloc(0x40);
    store<u8>(holder + 0x21, static_cast<u8>(g_rng.chance(0.8) ? 0x70 & g_rng.u32() : g_rng.u32()));
    store<u8>(holder + 0x22, static_cast<u8>(g_rng.chance(0.8) ? 0xe0 & g_rng.u32() : g_rng.u32()));
    u64* link = reinterpret_cast<u64*>(holder + kFirst);
    const int n = g_rng.chance(0.15) ? 0 : g_rng.range(1, 6);
    for (int i = 0; i < n; ++i) {
        const u64 node = alloc(0x60);
        store<u32>(node + kState, g_rng.chance(0.7) ? 0 : (g_rng.chance(0.5) ? g_rng.u32() : (1u << g_rng.range(0, 31))));
        if (!g_rng.chance(0.2)) {
            const u64 p = alloc(0x200);
            store<float>(p + kSoulRate, g_rng.value(-1, 4));
            store<float>(p + kHaveSoulRate, g_rng.value(-1, 4));
            static const u16 kKinds[] = {kStateDiscovery, kStateGathering, 0, 0x41, 0x43, 0x71, 0x73};
            store<u16>(p + kStateInfo,
                    g_rng.chance(0.8) ? kKinds[g_rng.range(0, 6)] : static_cast<u16>(g_rng.u32()));
            store<u8>(p + kClearBonusByte, static_cast<u8>(g_rng.u32()));
            store<float>(p + kItemDropRate, g_rng.value(-1, 2));
            store<u64>(node + kParam, p);
        }
        *link = node;
        link = reinterpret_cast<u64*>(node + kNext);
    }
    return holder;
}

// ---- The stubs both versions call -------------------------------------------

struct Call {
    int what;
    u64 a, b, c, d;
    bool operator==(const Call&) const = default;
};
std::vector<Call> g_calls;
u64 g_local_rax = 0, g_kill_rax = 0, g_row = 0;

extern "C" u64 stub_is_local(u64 self) {
    g_calls.push_back({1, self, 0, 0, 0});
    return g_local_rax;
}
extern "C" void stub_gain(u64 self, s32 amount, s32 third, u32 rates) {
    g_calls.push_back({2, self, static_cast<u32>(amount), static_cast<u32>(third), rates});
}
extern "C" u64 stub_kill_record(u64 a, u64 b, u32 c, u32 d) {
    g_calls.push_back({3, a, b, c, d});
    return g_kill_rax;
}
extern "C" void stub_param_row(ParamRef* out, s32 id) {
    g_calls.push_back({4, static_cast<u32>(id), 0, 0, 0});
    out->id = id;
    out->row = g_row;
}

// The game's function at `bn` jumps to `to` from here on, for both versions.
void redirect(u64 bn, void* to) {
    const u64 page = bn & ~0xfffull;
    if (mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) std::abort();
    std::uint8_t* p = eboot_kit::at(bn);
    p[0] = 0xff;
    p[1] = 0x25;
    std::memset(p + 2, 0, 4);
    const u64 a = reinterpret_cast<u64>(to);
    std::memcpy(p + 6, &a, 8);
}

// A player: its vtable (+0x1b8 is it local, +0x3d0 the gain), its effects,
// a record or none.
u64 player(u64 holder, u64 rec) {
    const u64 vt = alloc(0x800);
    store<u64>(vt + 0x1b8, reinterpret_cast<u64>(&stub_is_local));
    store<u64>(vt + 0x3d0, reinterpret_cast<u64>(&stub_gain));
    const u64 p = alloc(0x400);
    store<u64>(p, vt);
    store<u64>(p + 0x1c8, holder);
    store<u64>(p + 0x3c0, rec);
    return p;
}

u64 record() {
    const u64 r = alloc(0x100);
    store<s32>(r + kInsight, g_rng.chance(0.5) ? g_rng.range(0, 99) : any_int());
    store<s32>(r + kEchoes, any_int());
    store<u64>(r + kEchoesEver, g_rng.chance(0.5) ? static_cast<u64>(static_cast<u32>(any_int())) : g_rng.next());
    store<s32>(r + kEchoesGathered, any_int());
    return r;
}

void slot(u64 bn, u64 value) { store<u64>(bn, value); }

// ---- One case: the game's, then ours, from the same state -------------------

struct Run {
    std::vector<std::uint8_t> arena;
    std::vector<Call> calls;
    u64 ret;
    u32 fret;
};

template <class F>
Run run(F f) {
    g_calls.clear();
    Run r;
    r.ret = 0;
    r.fret = 0;
    f(r);
    r.arena.assign(g_arena, g_arena + g_used);
    r.calls = g_calls;
    return r;
}

int g_bad = 0;
int g_price_edges = 0;

template <class Game, class Ours>
bool same_case(const char* name, int c, Game game, Ours ours) {
    const std::vector<std::uint8_t> start(g_arena, g_arena + g_used);
    const Run theirs = run(game);
    std::memcpy(g_arena, start.data(), start.size());
    const Run mine = run(ours);
    std::size_t at = 0;
    while (at < theirs.arena.size() && theirs.arena[at] == mine.arena[at]) ++at;
    const bool ok = at == theirs.arena.size() && theirs.calls == mine.calls && theirs.ret == mine.ret && theirs.fret == mine.fret;
    if (!ok && ++g_bad <= 12) {
        std::printf("%s: case %d differs: ret 0x%llx / 0x%llx, float 0x%08x / 0x%08x, calls %zu / %zu, first byte at +0x%zx\n",
                    name, c, static_cast<ull>(theirs.ret), static_cast<ull>(mine.ret), theirs.fret, mine.fret,
                    theirs.calls.size(), mine.calls.size(), at);
    }
    return ok;
}

using DiscoveryGame = float (*)(u64, float);
using GainGame = void (*)(u64, s32, s32, u32);
using KillGame = u64 (*)(u64, u64, u32, u32, float);
using PenaltyGame = u64 (*)(u64, s32, float);
using PriceGame = u64 (*)(u64, const s32*, const s32*);

int discovery_cases(int n) {
    int bad = 0;
    for (int c = 0; c < n; ++c) {
        reset();
        const u64 holder = g_rng.chance(0.1) ? 0 : effects();
        static const float kEdges[] = {7.99f, 8.0f, 8.0001f, 29.99f, 30.0f, 30.0001f, 49.99f, 50.0f, 50.0001f, 99.0f, 0.0f, -0.0f,
                                       -1.0f};
        const float arcane = g_rng.chance(0.2) ? kEdges[g_rng.range(0, 12)] : g_rng.value(-5, 99);
        bad += !same_case("sub_1981830", c,
                          [&](Run& r) { r.fret = bits(eboot_kit::fn<DiscoveryGame>(0x1981830)(holder, arcane)); },
                          [&](Run& r) { r.fret = bits(item_discovery(holder, arcane)); });
    }
    return bad;
}

int gain_cases(int n) {
    int bad = 0;
    for (int c = 0; c < n; ++c) {
        reset();
        const u64 menu = alloc(0x300);
        store<s32>(menu + 0x20c, any_int());
        store<s32>(menu + 0x210, any_int());
        slot(kMenuMan, menu);
        const u64 p = player(effects(), g_rng.chance(0.1) ? 0 : record());
        g_local_rax = (g_rng.next() & ~0xffull) | (g_rng.chance(0.8) ? 1 : (g_rng.chance(0.5) ? 0 : 0x80));
        const s32 amount = any_int(), third = any_int();
        const u32 rates = any_flag();
        bad += !same_case("PlayerIns::vf122", c, [&](Run&) { eboot_kit::fn<GainGame>(0x1cfc600)(p, amount, third, rates); },
                          [&](Run&) { add_echoes(p, amount, third, rates); });
    }
    return bad;
}

int kill_cases(int n) {
    int bad = 0;
    for (int c = 0; c < n; ++c) {
        reset();
        const u64 man = alloc(0x100);
        static const s32 kClears[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, -1, INT32_MIN, INT32_MAX};
        store<s32>(man + 0x68, kClears[g_rng.range(0, 11)]);
        slot(kGameDataMan, man);
        const u64 p = player(effects(), record());
        u64 victim = 0;
        if (!g_rng.chance(0.1)) {
            victim = alloc(0x400);
            store<u64>(victim + 0x1c8, effects());
        }
        g_kill_rax = g_rng.next();
        const u32 coop = any_flag(), halve = any_flag();
        const float base = g_rng.chance(0.7) ? static_cast<float>(g_rng.range(0, 100000)) : g_rng.value(-10, 1e6f);
        bad += !same_case("sub_1cfc860", c,
                          [&](Run& r) { r.ret = eboot_kit::fn<KillGame>(0x1cfc860)(p, victim, coop, halve, base); },
                          [&](Run& r) { r.ret = kill_echoes(p, victim, coop, halve, base); });
    }
    return bad;
}

int penalty_cases(int n) {
    int bad = 0;
    for (int c = 0; c < n; ++c) {
        reset();
        const u64 man = alloc(0x100);
        store<u64>(man + 0x8, g_rng.chance(0.1) ? 0 : record());
        slot(kGameDataMan, man);
        slot(kLuaEventMan, g_rng.chance(0.1) ? 0 : alloc(0x10));
        const u64 a0 = g_rng.next();
        const s32 insight = g_rng.chance(0.6) ? g_rng.range(0, 10) : any_int();
        const float share = g_rng.chance(0.6) ? 1.0f : g_rng.value(-0.5f, 1.5f);
        bad += !same_case("lua_cli_ExcutePenalty", c,
                          [&](Run& r) { r.ret = eboot_kit::fn<PenaltyGame>(0x1734e40)(a0, insight, share); },
                          [&](Run& r) { r.ret = ExcutePenalty(a0, insight, share); });
    }
    return bad;
}

int price_cases(int n) {
    int bad = 0;
    for (int c = 0; c < n; ++c) {
        reset();
        const u64 man = alloc(0x100);
        const u64 rec = record();
        store<u64>(man + 0x8, rec);
        slot(kGameDataMan, man);
        const u64 screen = alloc(0x1000);
        store<s32>(screen + 0xe94, g_rng.chance(0.8) ? g_rng.range(1, 700) : any_int());
        store<s32>(screen + 0xeb0, g_rng.chance(0.5) ? 0 : any_int());
        const u64 floor = alloc(0x10);
        store<s32>(floor, g_rng.range(0, 99));
        const u64 ctx = alloc(0x20);
        store<u64>(ctx + 0x8, screen);
        store<u64>(ctx + 0x10, floor);
        g_row = 0;
        if (!g_rng.chance(0.15)) {
            g_row = alloc(0x60);
            store<float>(g_row + 0x3c, g_rng.value(-2, 2));
            store<float>(g_row + 0x40, g_rng.value(-100, 5000));
            store<float>(g_row + 0x44, g_rng.value(-1, 1));
            store<float>(g_row + 0x48, g_rng.value(-100, 300));
        }
        const u64 from = alloc(0x10), to = alloc(0x10);
        store<s32>(from, g_rng.chance(0.9) ? g_rng.range(0, 99) : any_int());
        store<s32>(to, g_rng.chance(0.9) ? g_rng.range(0, 101) : any_int());
        const auto* f = reinterpret_cast<const s32*>(from);
        const auto* t = reinterpret_cast<const s32*>(to);
        const auto game = eboot_kit::fn<PriceGame>(0x1f2f5e0);
        // Where the answer turns: the fewest echoes the game's version calls
        // enough (a binary search on its own answers), so ours is checked at
        // the price itself and either side of it, not only far from it.
        if (*t > *f && *t <= 99 && g_rng.chance(0.5)) {
            store<s32>(screen + 0xeb0, 0);
            std::int64_t lo = INT32_MIN, hi = INT32_MAX;
            while (lo < hi) {
                const std::int64_t mid = lo + (hi - lo) / 2;
                store<s32>(rec + kEchoes, static_cast<s32>(mid));
                if (static_cast<u32>(game(ctx, f, t)) == static_cast<u32>(*t)) hi = mid;
                else lo = mid + 1;
            }
            store<s32>(rec + kEchoes, static_cast<s32>(lo + g_rng.range(lo > INT32_MIN ? -1 : 0, lo < INT32_MAX ? 1 : 0)));
            ++g_price_edges;
        }
        bad += !same_case("sub_1f2f5e0", c, [&](Run& r) { r.ret = game(ctx, f, t); },
                          [&](Run& r) { r.ret = level_price_check(ctx, f, t); });
    }
    return bad;
}

}  // namespace

int main() {
    eboot_kit::load("decomp_player_data");
    struct Entry {
        u64 bn;
        const u8* bytes;
        std::size_t n;
    } const entries[] = {{0x1981830, kDiscoveryEntry, sizeof kDiscoveryEntry}, {0x1cfc600, kGainEntry, sizeof kGainEntry},
                         {0x1cfc860, kKillEntry, sizeof kKillEntry},           {0x1734e40, kPenaltyEntry, sizeof kPenaltyEntry},
                         {0x1f2f5e0, kPriceEntry, sizeof kPriceEntry}};
    for (const Entry& e : entries) {
        if (std::memcmp(eboot_kit::at(e.bn), e.bytes, e.n) != 0) {
            std::printf("decomp_player_data: 0x%llx is not the entry the decomp was written against\n", static_cast<ull>(e.bn));
            return 1;
        }
    }
    redirect(kKillRecord, reinterpret_cast<void*>(&stub_kill_record));
    redirect(kParamRow, reinterpret_cast<void*>(&stub_param_row));
    constexpr int kCases = 100000;
    int bad = 0;
    for (const bool specials : {false, true}) {
        g_rng.specials = specials;
        bad += discovery_cases(kCases);
        bad += gain_cases(kCases);
        bad += kill_cases(kCases);
        bad += penalty_cases(kCases);
        bad += price_cases(kCases);
    }
    std::printf("decomp_player_data: %d cases each of 5 functions (ordinary values, then special ones; %d price checks at the "
                "price), %d differ\n",
                2 * kCases, g_price_edges, bad);
    return bad ? 1 : 0;
}
