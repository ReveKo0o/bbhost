// The item-lot roll (decomp/items/item_lots.cpp) against the game's own
// (tests/decomp/eboot_kit.h): sub_1bceaf0 and ours on the same random lots - rows of
// every slot shape, counters at every count, discovery, category-15 levels,
// flags on and off, results to free - from the same flag store, generator and
// heap. The results are compared byte for byte, and so are the generator and
// every flag block afterwards, the heap hint, and what is left allocated.
// Skips without the 1.09 eboot (BBHOST_EBOOT, or eboot-109-decrypted.bin in
// the checkout).
#include "decomp/items/item_lots.cpp"

#include "eboot_kit.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>


namespace {

const char* const kTest = "decomp_item_lots";
eboot_kit::Rng g_rng(0x1bceaf0);

// ---- The heap and the allocator the game's trees use --------------------
//
// A heap object is a vtable pointer: +0x20 a query whose answer's first
// byte must carry 0x20, +0x58 allocate (self, size, align), +0x70 free.

std::set<void*> g_live;

std::uint8_t* heap_query(std::uint8_t* out, void*, std::uint32_t) {
    std::memset(out, 0, 16);
    out[0] = 0x20;
    return out;
}

void* heap_alloc(void*, std::uint64_t size, std::uint64_t align) {
    void* p = nullptr;
    if (posix_memalign(&p, align < 16 ? 16 : align, size ? size : 1) != 0) return nullptr;
    std::memset(p, 0xcd, size);
    g_live.insert(p);
    return p;
}

void heap_free(void*, void* p) {
    if (!g_live.erase(p)) {
        std::printf("%s: a free of a block the heap never gave (%p)\n", kTest, p);
        std::exit(1);
    }
    std::free(p);
}

[[noreturn]] void heap_other() {
    std::printf("%s: a heap method the roll should not call\n", kTest);
    std::exit(1);
}

const void* g_vtable[32];
struct Heap {
    const void* const* vt = g_vtable;
} g_heap;
alignas(16) std::uint8_t g_runtime[0x100];  // the thread's DLRuntime object: its allocator at +0x28

void* stub_find_heap(void*) { return &g_heap; }

// ---- Params ----------------------------------------------------------------

struct Lookup {
    std::int32_t index;
    std::int32_t pad;
    const void* row;
};

std::map<std::int32_t, std::array<std::uint8_t, 0x94>> g_param_rows;
std::map<std::pair<std::int32_t, std::uint32_t>, std::array<std::int32_t, 2>> g_param_lvdep;  // (id, level) -> {category, id}

void stub_lot_row(Lookup* out, std::int32_t id) {
    if (auto it = g_param_rows.find(id); it != g_param_rows.end()) out->row = it->second.data();
}

void stub_lvdep_row(Lookup* out, std::int32_t id, std::uint32_t level) {
    if (auto it = g_param_lvdep.find({id, level & 0xff}); it != g_param_lvdep.end()) out->row = it->second.data();
}

// ---- The flag store --------------------------------------------------------
//
// SprjEventFlagMan (decomp/events/flag_store.h): blocks of kBlock flags in
// a tree keyed by block number, stored by pointer (kind 2) or in a pool
// (kind 1).

constexpr std::uint32_t kBlock = 1000, kBlockBytes = kBlock / 8;
struct Node {
    Node* left;
    Node* parent;
    Node* right;
    std::uint8_t color, nil, pad[6];
    std::uint32_t key, pad2;
    std::uint32_t kind, pad3;
    std::uint64_t data;
};
static_assert(sizeof(Node) == 0x38 && offsetof(Node, key) == 0x20 && offsetof(Node, data) == 0x30, "the store's map nodes");

const std::uint32_t kBlocks[] = {0, 1, 2, 3, 5, 8, 9, 20};  // 4, 6, 7 missing
alignas(16) std::uint8_t g_man[0x40];
Node g_head;
Node g_nodes[std::size(kBlocks)];
std::uint8_t g_storage[std::size(kBlocks)][kBlockBytes];
std::uint8_t g_pool[4][kBlockBytes];  // kind-1 blocks live here

Node* build(int lo, int hi) {
    if (lo >= hi) return &g_head;
    const int mid = (lo + hi) / 2;
    Node& n = g_nodes[mid];
    n.left = build(lo, mid);
    n.right = build(mid + 1, hi);
    return &n;
}

void make_store() {
    g_head = {};
    g_head.nil = 1;
    int pooled = 0;
    for (std::size_t i = 0; i < std::size(kBlocks); ++i) {
        Node& n = g_nodes[i];
        n = {};
        n.key = kBlocks[i];
        if (i % 3 == 1 && pooled < 4) {
            n.kind = 1;
            n.data = static_cast<std::uint64_t>(pooled++);
        } else {
            n.kind = 2;
            n.data = reinterpret_cast<std::uint64_t>(g_storage[i]);
        }
    }
    g_head.parent = build(0, static_cast<int>(std::size(kBlocks)));
    std::memset(g_man, 0, sizeof g_man);
    const std::uint32_t size = kBlock, stride = kBlockBytes;
    const std::uint64_t pool = reinterpret_cast<std::uint64_t>(g_pool), head = reinterpret_cast<std::uint64_t>(&g_head);
    std::memcpy(g_man + 0x1c, &size, 4);
    std::memcpy(g_man + 0x20, &stride, 4);
    std::memcpy(g_man + 0x28, &pool, 8);
    std::memcpy(g_man + 0x38, &head, 8);
}

// A flag id: mostly in a block, near a block's edges, in a missing block,
// or below zero.
std::int32_t random_flag() {
    if (g_rng.chance(0.08)) return -g_rng.range(1, 3000);
    if (g_rng.chance(0.08)) return static_cast<std::int32_t>(kBlock * g_rng.range(4, 7) + g_rng.range(0, static_cast<int>(kBlock) - 1));
    const std::uint32_t block = kBlocks[g_rng.range(0, static_cast<int>(std::size(kBlocks)) - 1)];
    const int off = g_rng.chance(0.2) ? (g_rng.chance(0.5) ? g_rng.range(0, 7) : g_rng.range(static_cast<int>(kBlock) - 8, static_cast<int>(kBlock) - 1)) : g_rng.range(0, static_cast<int>(kBlock) - 1);
    return static_cast<std::int32_t>(block * kBlock + off);
}

// ---- The generator ---------------------------------------------------------

alignas(16) std::uint8_t g_state[0x1500];

void random_generator() {
    std::uint8_t* mt = g_state + 0xa78;
    for (int i = 0; i < 624; ++i) {
        const std::uint32_t w = g_rng.u32();
        std::memcpy(mt + 0x10 + 4 * i, &w, 4);
    }
    // Words left: now and then 1, so the next draw refills.
    const std::uint32_t left = g_rng.chance(0.1) ? 1 : static_cast<std::uint32_t>(g_rng.range(1, 624));
    const std::uint64_t next = reinterpret_cast<std::uint64_t>(mt + 0x10 + 4 * (624 - left));
    std::memcpy(mt + 4, &left, 4);
    std::memcpy(mt + 8, &next, 8);
}

// ---- A case ----------------------------------------------------------------

struct TestContext {
    std::int32_t lot;
    float discovery;
    std::uint8_t honour, offset, level, pad;
    std::uint32_t count;
    std::uint64_t results;
};

std::uint16_t points() {
    if (g_rng.chance(0.4)) return 0;
    if (g_rng.chance(0.05)) return 65535;
    return static_cast<std::uint16_t>(g_rng.range(1, g_rng.chance(0.5) ? 100 : 5000));
}

void random_row(std::array<std::uint8_t, 0x94>& row) {
    for (auto& b : row) b = static_cast<std::uint8_t>(g_rng.u32());
    const auto put32 = [&](int at, std::int32_t v) { std::memcpy(row.data() + at, &v, 4); };
    const auto put16 = [&](int at, std::uint16_t v) { std::memcpy(row.data() + at, &v, 2); };
    for (int i = 0; i < 8; ++i) {
        put32(4 * i, g_rng.chance(0.05) ? -g_rng.range(1, 5) : g_rng.range(0, 3000));
        static const std::int32_t kCats[] = {0, 1, 2, 4, 8, 15, 15, 3, -1};
        put32(0x20 + 4 * i, kCats[g_rng.range(0, static_cast<int>(std::size(kCats)) - 1)]);
        put16(0x40 + 2 * i, points());
        put16(0x50 + 2 * i, points());
        put32(0x60 + 4 * i, g_rng.chance(0.3) ? 0 : random_flag());
        row[0x8a + i] = static_cast<std::uint8_t>(g_rng.chance(0.15) ? 0 : g_rng.range(1, 5));
    }
    put32(0x80, g_rng.chance(0.2) ? 0 : random_flag());
    put32(0x84, g_rng.chance(0.3) ? -1 : random_flag());
    row[0x88] = static_cast<std::uint8_t>(g_rng.chance(0.4) ? 0 : g_rng.chance(0.5) ? g_rng.range(1, 10) : g_rng.range(0, 255));
    row[0x89] = static_cast<std::uint8_t>(g_rng.u32());
    put16(0x92, static_cast<std::uint16_t>(g_rng.u32()));
}

struct World {
    std::vector<std::uint8_t> storage, pool, state;
    std::uint64_t hint;
};

std::uint64_t& hint() {
    return *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(eboot_kit::tcb()) - 0x720 + 0x10);
}

World snapshot() {
    World w;
    w.storage.assign(&g_storage[0][0], &g_storage[0][0] + sizeof g_storage);
    w.pool.assign(&g_pool[0][0], &g_pool[0][0] + sizeof g_pool);
    w.state.assign(g_state, g_state + sizeof g_state);
    w.hint = hint();
    return w;
}

void restore(const World& w) {
    std::memcpy(g_storage, w.storage.data(), w.storage.size());
    std::memcpy(g_pool, w.pool.data(), w.pool.size());
    std::memcpy(g_state, w.state.data(), w.state.size());
    hint() = w.hint;
}

struct Outcome {
    std::uint32_t count;
    std::vector<std::uint8_t> results;
    World after;
};

// An old result buffer, as a caller hands the roll one to replace.
std::uint64_t old_results(bool any) { return any ? reinterpret_cast<std::uint64_t>(heap_alloc(&g_heap, 0x50 * 3, 4)) : 0; }

Outcome run(bool game, const TestContext& base, bool old) {
    TestContext ctx = base;
    ctx.results = old_results(old);
    if (game)
        eboot_kit::fn<void (*)(TestContext*, std::uint32_t, void*)>(0x1bceaf0)(&ctx, 1, nullptr);
    else
        item_lot::roll(reinterpret_cast<std::uint64_t>(&ctx), 1, 0);
    Outcome o;
    o.count = ctx.count;
    if (ctx.results) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(ctx.results);
        o.results.assign(p, p + 0x50 * static_cast<std::size_t>(ctx.count));
        heap_free(&g_heap, reinterpret_cast<void*>(ctx.results));
    }
    o.after = snapshot();
    return o;
}

int g_bad = 0;
// How much of the roll the cases reached, from the game's side.
struct Reach {
    long results = 0, entries = 0, merged = 0, full = 0, refills = 0, counters = 0, freed_hint = 0, freed_found = 0, unflagged = 0;
} g_reach;

void one_case(int c) {
    g_param_rows.clear();
    g_param_lvdep.clear();
    const std::int32_t lot = g_rng.chance(0.5) ? 1000 : g_rng.range(1, 900000) * 10;
    // Now and then a long chain whose slots all share the row's flag, so the
    // picks merge into one result past its six items.
    const bool shared = g_rng.chance(0.15);
    const int rows = g_rng.chance(0.05) ? 0 : shared ? g_rng.range(6, 12) : g_rng.range(1, g_rng.chance(0.2) ? 8 : 3);
    const std::int32_t common = random_flag();
    for (int r = 0; r < rows; ++r) {
        auto& row = g_param_rows[lot + r];
        random_row(row);
        if (!shared) continue;
        std::memset(row.data() + 0x60, 0, 0x20);
        std::memcpy(row.data() + 0x80, &common, 4);
    }
    if (g_rng.chance(0.1)) random_row(g_param_rows[lot + rows + 1]);  // after a gap: never read
    for (const auto& [id, row] : g_param_rows)
        for (int i = 0; i < 8; ++i) {
            std::int32_t cat, item;
            std::memcpy(&cat, row.data() + 0x20 + 4 * i, 4);
            std::memcpy(&item, row.data() + 4 * i, 4);
            if (cat == 15 && g_rng.chance(0.7)) {
                const std::uint32_t level = g_rng.chance(0.7) ? 0 : static_cast<std::uint32_t>(g_rng.range(0, 255));
                g_param_lvdep[{item, level}] = {g_rng.chance(0.1) ? 15 : g_rng.range(0, 4), g_rng.chance(0.1) ? -1 : g_rng.range(0, 3000)};
            }
        }
    for (auto& b : g_storage) for (auto& x : b) x = static_cast<std::uint8_t>(g_rng.u32());
    for (auto& b : g_pool) for (auto& x : b) x = static_cast<std::uint8_t>(g_rng.u32());
    random_generator();
    TestContext ctx{};
    ctx.lot = g_rng.chance(0.03) ? lot + 50 : lot;
    ctx.discovery = g_rng.value(0.5f, 3.0f);
    ctx.honour = static_cast<std::uint8_t>(g_rng.chance(0.6) ? 1 : g_rng.chance(0.5) ? 0 : g_rng.range(2, 255));
    ctx.offset = static_cast<std::uint8_t>(g_rng.chance(0.8) ? 0 : g_rng.range(1, 255));
    ctx.level = static_cast<std::uint8_t>(g_rng.chance(0.7) ? 0 : g_rng.range(0, 255));
    const bool old = g_rng.chance(0.3);
    hint() = old && g_rng.chance(0.5) ? reinterpret_cast<std::uint64_t>(&g_heap) : 0;

    const World before = snapshot();
    const Outcome theirs = run(true, ctx, old);
    if (theirs.count) ++g_reach.results;
    g_reach.entries += theirs.count;
    for (std::uint32_t e = 0; e < theirs.count; ++e) {
        const std::uint8_t n = theirs.results[0x50 * e + 5];
        if (n > 1) ++g_reach.merged;
        if (n == 6) ++g_reach.full;
        std::int32_t flag;
        std::memcpy(&flag, &theirs.results[0x50 * e], 4);
        if (flag == -1 && !ctx.honour) ++g_reach.unflagged;
    }
    std::uint32_t left_before, left_after;
    std::memcpy(&left_before, before.state.data() + 0xa78 + 4, 4);
    std::memcpy(&left_after, theirs.after.state.data() + 0xa78 + 4, 4);
    if (left_after > left_before) ++g_reach.refills;
    if (theirs.after.storage != before.storage || theirs.after.pool != before.pool) ++g_reach.counters;
    if (old && before.hint) ++g_reach.freed_hint;
    if (old && !before.hint) ++g_reach.freed_found;
    restore(before);
    const Outcome ours = run(false, ctx, old);
    const bool same = theirs.count == ours.count && theirs.results == ours.results && theirs.after.storage == ours.after.storage &&
                      theirs.after.pool == ours.after.pool && theirs.after.state == ours.after.state && theirs.after.hint == ours.after.hint;
    if (!same && ++g_bad <= 8) {
        std::printf("case %d (row %d, %d rows, honour %u, offset %u, level %u, discovery %g): results %u/%u%s%s%s%s\n", c, ctx.lot, rows,
                    ctx.honour, ctx.offset, ctx.level, static_cast<double>(ctx.discovery), theirs.count, ours.count,
                    theirs.results == ours.results ? "" : ", results differ", theirs.after.state == ours.after.state ? "" : ", generator differs",
                    theirs.after.storage == ours.after.storage && theirs.after.pool == ours.after.pool ? "" : ", flags differ",
                    theirs.after.hint == ours.after.hint ? "" : ", heap hint differs");
    }
    if (!g_live.empty()) {
        std::printf("case %d: %zu blocks left allocated\n", c, g_live.size());
        std::exit(1);
    }
}

}  // namespace

int main() {
    eboot_kit::load(kTest);
    for (auto& v : g_vtable) v = reinterpret_cast<const void*>(&heap_other);
    g_vtable[0x20 / 8] = reinterpret_cast<const void*>(&heap_query);
    g_vtable[0x58 / 8] = reinterpret_cast<const void*>(&heap_alloc);
    g_vtable[0x70 / 8] = reinterpret_cast<const void*>(&heap_free);
    const void* vt = g_vtable;
    std::memcpy(g_runtime + 0x28, &vt, 8);
    auto* tls = static_cast<std::uint8_t*>(eboot_kit::tcb());
    const void* runtime = g_runtime;
    std::memcpy(tls - 0x730, &runtime, 8);
    t_guest_fs = tls;
    make_store();
    const auto set_slot = [](std::uint64_t bn, const void* v) { std::memcpy(eboot_kit::at(bn), &v, 8); };
    set_slot(0x5940420, &g_heap);
    set_slot(0x593b100, g_man);
    set_slot(0x5956678, g_state);
    *eboot_kit::at(0x593d6d8) = 0;
    eboot_kit::stub(0x2320640, reinterpret_cast<const void*>(&stub_lot_row));
    eboot_kit::stub(0x23202a0, reinterpret_cast<const void*>(&stub_lvdep_row));
    eboot_kit::stub(0x247b720, reinterpret_cast<const void*>(&stub_find_heap));

    constexpr int kCases = 100000;
    for (const bool specials : {false, true}) {
        g_rng.specials = specials;
        for (int c = 0; c < kCases; ++c) one_case(c);
    }
    std::printf("%s: reached - %ld rolls with results (%ld entries, %ld merged, %ld full, %ld without flags), %ld refills, %ld "
                "with counters written, old results freed through the hint %ld / found %ld\n",
                kTest, g_reach.results, g_reach.entries, g_reach.merged, g_reach.full, g_reach.unflagged, g_reach.refills,
                g_reach.counters, g_reach.freed_hint, g_reach.freed_found);
    std::printf("%s: %d lots each way (ordinary values, then special ones), %d differ\n", kTest, kCases, g_bad);
    return g_bad ? 1 : 0;
}
