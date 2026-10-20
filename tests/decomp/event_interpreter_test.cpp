// The event-script interpreter (decomp/events/interpreter.cpp) against the game's own
// code (tests/decomp/eboot_kit.h). Its eight functions take the game's place at their
// entries for "ours" and give it back for "the game's", so every caller -
// the game's own code included - reaches the version under test, as in the
// game. Everything they touch lives in one arena (the events, their scripts,
// a fake heap, the EMK system, the flag store), so a step can be run twice
// from the same bytes and the results compared byte for byte:
//
//   - the dispatcher over every bank from -2 to 2100 (and the extremes), with
//     each bank function replaced by a recorder: the same function, the same
//     arguments (dt to the bit), the same result;
//   - the holder's queries, its latch pass and its update over random
//     condition groups (constants, timers, group-state conditions);
//   - the interpreter loop on random scripts of scripted instructions - add a
//     condition, skip, end, restart, yield, suppress the broadcast, clear the
//     latched bits, a label, an unknown bank, the inline 2012 - run tick by
//     tick with conditions flipping between ticks: every byte of the arena,
//     and every broadcast, restart record and free in order.
//
// The game's Advance, Clear, AddCondition, the conditions' own updates and the
// behaviour digit run as they are; the heap, the event-sync broadcast, the
// restart queue and DL_PANIC are fakes that record. Skips without the 1.09
// eboot (BBHOST_EBOOT, or eboot-109-decrypted.bin in the checkout).
#include "decomp/events/interpreter.cpp"

#include "eboot_kit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/mman.h>

// What the decomp file links against in the game.
extern "C" GUEST_ABI std::int64_t hle_call_guest6(void* fn, std::int64_t a0, std::int64_t a1, std::int64_t a2, std::int64_t a3,
                                                  std::int64_t a4, std::int64_t a5) {
    return reinterpret_cast<GUEST_ABI std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                                                       std::int64_t)>(fn)(a0, a1, a2, a3, a4, a5);
}

namespace {

using eboot_kit::at;

const char* const kTest = "decomp_event_interpreter";
int g_bad = 0;

// ---- The image: entries placed and restored -------------------------------

void writable(std::uint64_t bn, std::size_t n) {
    const std::uint64_t lo = bn & ~0xfffull, hi = (bn + n + 0xfff) & ~0xfffull;
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        std::printf("%s: cannot unprotect 0x%llx\n", kTest, static_cast<unsigned long long>(bn));
        std::exit(1);
    }
}

void jump(std::uint64_t bn, const void* to) {
    writable(bn, 14);
    std::uint8_t* p = at(bn);
    const auto d = reinterpret_cast<std::uint64_t>(to);
    p[0] = 0xff;
    p[1] = 0x25;
    std::memset(p + 2, 0, 4);
    std::memcpy(p + 6, &d, 8);
}

struct Entry {
    std::uint64_t bn;
    const std::uint8_t* bytes;
    std::size_t len;
    const void* ours;
};
const Entry kEntries[] = {
    {0x1bb93a0, kDispatchEntry, sizeof(kDispatchEntry), reinterpret_cast<const void*>(&emevd_dispatch_instruction)},
    {0x16ecdc0, kUpdateEntry, sizeof(kUpdateEntry), reinterpret_cast<const void*>(&SprjEmkEventIns_Update)},
    {0x16ed100, kEndEntry, sizeof(kEndEntry), reinterpret_cast<const void*>(&SprjEmkEventIns_EndOrRestart)},
    {0x16ec380, kHolderUpdateEntry, sizeof(kHolderUpdateEntry), reinterpret_cast<const void*>(&SprjEmkConditionHolder_Update)},
    {0x16ec4a0, kLatchEntry, sizeof(kLatchEntry), reinterpret_cast<const void*>(&SprjEmkConditionHolder_LatchGroupBits)},
    {0x16ec5a0, kQueryGroupEntry, sizeof(kQueryGroupEntry), reinterpret_cast<const void*>(&SprjEmkConditionHolder_QueryGroup)},
    {0x16ec660, kQueryMainEntry, sizeof(kQueryMainEntry), reinterpret_cast<const void*>(&SprjEmkConditionHolder_QueryMain)},
    {0x16ebcf0, kGroupQueryEntry, sizeof(kGroupQueryEntry), reinterpret_cast<const void*>(&SprjEmkConditionGroup_Query)},
};

void place_ours() {
    for (const Entry& e : kEntries) jump(e.bn, e.ours);
}

void place_game() {
    for (const Entry& e : kEntries) {
        writable(e.bn, e.len);
        std::memcpy(at(e.bn), e.bytes, e.len);
    }
}

// ---- The arena ------------------------------------------------------------

constexpr std::size_t kArena = 1u << 20, kFixed = 64 * 1024;
alignas(64) std::uint8_t g_arena[kArena];
std::vector<std::uint8_t> g_snap, g_theirs, g_ours;

// Its fixed part: the heap's top, the log, the fakes.
struct Fixed {
    std::uint64_t top;  // the heap's next free byte (an arena offset)
    std::uint64_t log_n;
    std::uint64_t log[4096];
    alignas(16) std::uint8_t heap_object[16];
    alignas(16) std::uint8_t system[0x78];
    alignas(16) std::uint8_t dispatcher[0x78];
    alignas(16) std::uint8_t flag_man[0x40];
    alignas(16) std::uint8_t map_head[0x40];
    alignas(16) std::uint8_t map_node[0x40];
    alignas(16) std::uint8_t flags[1024];
};
static_assert(sizeof(Fixed) <= kFixed, "the fixed part fits");
Fixed& fx() { return *reinterpret_cast<Fixed*>(g_arena); }

void log(std::uint64_t a, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) {
    Fixed& f = fx();
    if (f.log_n + 4 > std::size(f.log)) return;
    f.log[f.log_n++] = a;
    f.log[f.log_n++] = b;
    f.log[f.log_n++] = c;
    f.log[f.log_n++] = d;
}

std::uint64_t used() { return fx().top; }

// The bytes up to the heap's top; beyond it, a fill both runs start from.
void snapshot() {
    g_snap.assign(g_arena, g_arena + used());
    const std::size_t pad = std::min<std::size_t>(64 * 1024, kArena - g_snap.size());
    std::memset(g_arena + g_snap.size(), 0xcd, pad);
}
void restore() {
    std::memcpy(g_arena, g_snap.data(), g_snap.size());
    const std::size_t pad = std::min<std::size_t>(64 * 1024, kArena - g_snap.size());
    std::memset(g_arena + g_snap.size(), 0xcd, pad);  // beyond the top, the same for both
}
void capture(std::vector<std::uint8_t>& out) { out.assign(g_arena, g_arena + used()); }

bool same(const char* what, int a, int b) {
    if (g_theirs == g_ours) return true;
    std::size_t k = 0;
    while (k < g_theirs.size() && k < g_ours.size() && g_theirs[k] == g_ours[k]) ++k;
    if (++g_bad <= 12) {
        std::printf("%s (%d, %d): the arenas differ at +0x%zx (sizes 0x%zx, 0x%zx): the game's %02x, ours %02x\n", what, a, b, k,
                    g_theirs.size(), g_ours.size(), k < g_theirs.size() ? g_theirs[k] : 0, k < g_ours.size() ? g_ours[k] : 0);
    }
    return false;
}

// The heap: a bump allocator in the arena; frees are logged.
GUEST_ABI void* heap_alloc(void*, std::uint64_t size, std::uint64_t align) {
    Fixed& f = fx();
    const std::uint64_t a = align < 8 ? 8 : align;
    const std::uint64_t off = (f.top + a - 1) & ~(a - 1);
    if (off + size > kArena) {
        std::printf("%s: the arena is full\n", kTest);
        std::exit(1);
    }
    f.top = off + size;
    std::memset(g_arena + off, 0xab, size);
    log(1, off, size, align);
    return g_arena + off;
}
GUEST_ABI void heap_free(void*, void* p) { log(2, static_cast<std::uint8_t*>(p) - g_arena); }
void* g_heap_vtable[32];
GUEST_ABI void* heap_of(void*) { return fx().heap_object; }

// The game's helpers that reach the world, as recorders.
GUEST_ABI void broadcast(Ev* ev) { log(3, static_cast<std::uint64_t>(ev->core.event_id), static_cast<std::uint32_t>(ev->core.conditions.current_instruction), *ev->core.state); }
GUEST_ABI void enqueue_restart(void* system, const std::uint8_t* rec) {
    std::uint64_t args = 0;
    const std::uint8_t* copy;
    std::memcpy(&copy, rec + 0x10, 8);
    std::uint32_t size;
    std::memcpy(&size, rec + 0x18, 4);
    for (std::uint32_t i = 0; i < size && i < 8; ++i) args |= static_cast<std::uint64_t>(copy[i]) << (8 * i);
    std::uint64_t a, b;
    std::memcpy(&a, rec, 8);
    std::memcpy(&b, rec + 0x18, 8);
    std::uint64_t rd;
    std::memcpy(&rd, rec + 0x20, 8);
    log(4, a, b, args ^ rd ^ reinterpret_cast<std::uint64_t>(system));
}
GUEST_ABI void dl_panic(const char* file, int line, const char* fmt, const char* what) {
    std::printf("%s: DL_PANIC %s:%d %s\n", kTest, file, line, what);
    std::exit(1);
}

// ---- The bank functions ---------------------------------------------------

constexpr std::uint64_t kBanks[] = {0x1bc7160, 0x1bc8630, 0x1bbd1d0, 0x1bb9c50, 0x1bc40d0, 0x1bc2f60, 0x1bc75b0, 0x1bc8a90,
                                    0x1bbedd0, 0x1bc43f0, 0x1bc8030, 0x1bc53b0, 0x1bc0420, 0x1bba6a0, 0x1bc46e0, 0x1bc64e0,
                                    0x1bc37a0, 0x1bb97d0, 0x1bc5d90, 0x1bc6ad0, 0x1bc31e0, 0x16eb3b0};
bool g_scripted = false;
constexpr std::uint64_t kConstantVt = 0x5786210, kTimerVt = 0x572d470, kGroupStateVt = 0x572ca70;
constexpr std::uint64_t kAddCondition = 0x16ec230;

const std::uint8_t* instruction_args(const Ev* ev) {
    if (ev->instruction_arguments) return ev->instruction_arguments;
    const std::uint8_t* image = ev->instruction_source->data;
    std::int64_t blob;
    std::memcpy(&blob, image + 0x78, 8);
    return image + blob + ev->instruction->arguments_offset;
}

// A producer, as the game's condition instructions build theirs: allocated
// from the default heap, the prefix set, updated once with no time, added.
void add_condition(Ev* ev, std::int8_t group, std::uint64_t vtable, const std::uint8_t* fields, std::size_t n) {
    void* heap = *reinterpret_cast<void**>(at(kDefaultHeapSlot));
    auto* c = static_cast<std::uint8_t*>(virt<AllocFn>(heap, 0x58)(heap, 0x30, 8));
    std::memset(c, 0, 0x30);
    const std::uint64_t vt = vtable;
    std::memcpy(c, &vt, 8);
    const std::int32_t minus1 = -1;
    std::memcpy(c + 0x14, &minus1, 4);
    std::memcpy(c + 0x28, fields, n);
    const bb::FD4Time zero{0, 0.0f};
    virt<GroupUpdateFn>(c, 0x10)(reinterpret_cast<Group*>(c), &zero, ev);
    reinterpret_cast<GUEST_ABI void (*)(Holder*, std::uint32_t, void*)>(kAddCondition)(&ev->core.conditions,
                                                                                       static_cast<std::uint8_t>(group), c);
}

std::uint8_t script(int bank_index, void* d, Ev* ev, float dt) {
    const std::int32_t bank = ev->instruction->bank, id = ev->instruction->instruction_id;
    std::uint32_t dt_bits;
    std::memcpy(&dt_bits, &dt, 4);
    log(5, static_cast<std::uint64_t>(bank_index) << 32 | static_cast<std::uint32_t>(id),
        reinterpret_cast<std::uint64_t>(d) ^ reinterpret_cast<std::uint64_t>(ev), dt_bits);
    if (!g_scripted) return static_cast<std::uint8_t>(0x40 + bank_index);
    const std::uint8_t* a = instruction_args(ev);
    if (bank == 0 && id == 1) {  // a constant
        const std::uint8_t v = a[1] & 1;
        add_condition(ev, static_cast<std::int8_t>(a[0]), kConstantVt, &v, 1);
    } else if (bank == 0 && id == 0) {  // a group's state: mode 0, desired, first update, target
        const std::uint8_t f[8] = {0, static_cast<std::uint8_t>(a[1] & 1), 0, 1, a[2], 0, 0, 0};
        add_condition(ev, static_cast<std::int8_t>(a[0]), kGroupStateVt, f, 8);
    } else if (bank == 1 && id == 0) {  // a timer
        float seconds = static_cast<float>(a[1] % 4) * 0.0333333351f;
        std::uint8_t f[8] = {};
        std::memcpy(f, &seconds, 4);
        f[4] = 1;
        add_condition(ev, static_cast<std::int8_t>(a[0]), kTimerVt, f, 8);
    } else if (bank == 1000 && id == 3) {  // skip
        ev->instruction_delta = static_cast<std::int32_t>(a[0] % 3) + 1;
    } else if (bank == 1000 && id == 4) {  // end
        virt<EndFn>(ev, 0x30)(ev, 0);
    } else if (bank == 1000 && id == 2) {  // restart
        virt<EndFn>(ev, 0x30)(ev, 1);
    } else if (bank == 2000 && id == 3) {  // the latched bits cleared
        *ev->core.state = 0;
    } else if (bank == 5 && id == 0) {  // the next broadcast suppressed
        ev->suppress_callback_once = 1;
    } else if (bank == 11 && id == 0) {  // yield
        ev->continue_update = 0;
    }
    return static_cast<std::uint8_t>(a[0] & 1);
}

template <int I>
GUEST_ABI std::uint8_t bank_stub(void* d, Ev* ev, float dt) {
    return script(I, d, ev, dt);
}
template <int... I>
void stub_banks(std::integer_sequence<int, I...>) {
    (jump(kBanks[I], reinterpret_cast<const void*>(&bank_stub<I>)), ...);
}
GUEST_ABI std::uint64_t draw_stub(std::int32_t id, std::uint32_t on) {
    log(6, static_cast<std::uint32_t>(id), on);
    return 0x77;
}

// ---- The world the events see ---------------------------------------------

constexpr std::uint32_t kBlockSize = 8192;

void reset_arena() {
    std::memset(g_arena, 0, kArena);
    Fixed& f = fx();
    f.top = kFixed;
    const void* vt = g_heap_vtable;
    std::memcpy(f.heap_object, &vt, 8);
    // SprjEmkSystem: the dispatcher at +0x38, debug off (+0x70).
    void* d = f.dispatcher;
    std::memcpy(f.system + 0x38, &d, 8);
    // SprjEventFlagMan: one block (0) of 8192 flags, stored by pointer.
    std::memcpy(f.flag_man + 0x1c, &kBlockSize, 4);
    void* head = f.map_head;
    std::memcpy(f.flag_man + 0x38, &head, 8);
    void* node = f.map_node;
    std::memcpy(f.map_head + 8, &node, 8);
    f.map_head[0x19] = 1;  // the head is nil
    std::memcpy(f.map_node + 0x00, &head, 8);  // left
    std::memcpy(f.map_node + 0x10, &head, 8);  // right
    f.map_node[0x19] = 0;
    const std::uint32_t block = 0, kind = 2;
    std::memcpy(f.map_node + 0x20, &block, 4);
    std::memcpy(f.map_node + 0x28, &kind, 4);
    void* storage = f.flags;
    std::memcpy(f.map_node + 0x30, &storage, 8);
}

void set_globals() {
    Fixed& f = fx();
    void* p = f.heap_object;
    std::memcpy(at(kDefaultHeapSlot), &p, 8);
    p = f.system;
    std::memcpy(at(kEmkSystemSlot), &p, 8);
    p = f.flag_man;
    std::memcpy(at(kEventFlagManSlot), &p, 8);
}

// ---- 1. The dispatcher ----------------------------------------------------

void test_dispatch(eboot_kit::Rng& rng) {
    g_scripted = false;
    std::vector<std::int32_t> banks;
    for (std::int32_t b = -2; b <= 2100; ++b) banks.push_back(b);
    for (std::int32_t b : {INT32_MIN, -1000, -1, 2999, 9000, 65535, INT32_MAX}) banks.push_back(b);
    const std::int32_t ids[] = {-1, 0, 1, 2, 3, 101, INT32_MAX};
    int cases = 0;
    for (std::int32_t bank : banks) {
        for (std::int32_t id : ids) {
            for (int path = 0; path < (bank == 2012 ? 2 : 1); ++path) {
                reset_arena();
                Fixed& f = fx();
                auto* ev = reinterpret_cast<Ev*>(heap_alloc(nullptr, sizeof(Ev), 16));
                std::memset(ev, 0, sizeof(Ev));
                auto* in = reinterpret_cast<bb::SprjEmkInstructionRecord*>(heap_alloc(nullptr, 0x20, 8));
                std::memset(in, 0, 0x20);
                in->bank = bank;
                in->instruction_id = id;
                // The arguments: the event's copy, or the image's blob.
                auto* args = static_cast<std::uint8_t*>(heap_alloc(nullptr, 16, 16));
                const std::int32_t value = static_cast<std::int32_t>(rng.u32());
                std::memcpy(args, &value, 4);
                args[4] = static_cast<std::uint8_t>(rng.range(0, 3));
                auto* image = static_cast<std::uint8_t*>(heap_alloc(nullptr, 0x100, 16));
                std::memset(image, 0, 0x100);
                const std::int64_t blob = 0x40;
                std::memcpy(image + 0x78, &blob, 8);
                in->arguments_offset = (args - image) - blob;
                auto* rd = reinterpret_cast<bb::SprjEvdRuntimeData*>(heap_alloc(nullptr, 0x38, 8));
                std::memset(rd, 0, 0x38);
                rd->data = image;
                ev->instruction = in;
                ev->instruction_source = rd;
                ev->instruction_arguments = path ? nullptr : args;
                const float dt = rng.chance(0.2) ? rng.value(-1, 1) : rng.uniform(0, 0.1f);
                (void)f;
                snapshot();
                place_game();
                const auto game = reinterpret_cast<DispatchFn>(0x1bb93a0);
                const std::uint8_t theirs = game(fx().dispatcher, ev, dt);
                capture(g_theirs);
                restore();
                place_ours();
                const std::uint8_t ours = game(fx().dispatcher, ev, dt);
                capture(g_ours);
                if (theirs != ours && ++g_bad <= 12)
                    std::printf("dispatch bank %d id %d: the game's %u, ours %u\n", bank, id, theirs, ours);
                same("dispatch", bank, id);
                ++cases;
            }
        }
    }
    std::printf("%s: dispatch: %d instructions, every bank from -2 to 2100 and the extremes\n", kTest, cases);
}

// ---- 2. The holder ----------------------------------------------------------

Ev* new_event(eboot_kit::Rng& rng) {
    auto* ev = reinterpret_cast<Ev*>(heap_alloc(nullptr, sizeof(Ev), 16));
    std::memset(ev, 0, sizeof(Ev));
    const std::uint64_t vt = 0x5766810;  // SprjEmkEventIns
    std::memcpy(ev, &vt, 8);
    ev->core.state = ev->core.inline_state;
    ev->core.event_id = rng.chance(0.1) ? -rng.range(1, 5) : rng.range(50, 160);
    ev->core.slot = static_cast<std::int16_t>(rng.chance(0.1) ? -1 : rng.range(0, 3));
    ev->core.conditions.current_instruction = -1;
    ev->core.conditions.remote_instruction = -1;
    ev->core.network_source = 10000;
    ev->core.map_id = rng.u32();
    ev->flag_78 = 1;
    return ev;
}

// Random groups of constants, timers and group-state conditions, through the
// game's AddCondition; then random results and overrides.
void random_conditions(eboot_kit::Rng& rng, Ev* ev) {
    const int n = rng.range(0, 14);
    for (int i = 0; i < n; ++i) {
        const std::int8_t g = static_cast<std::int8_t>(rng.chance(0.05) ? rng.range(-128, 127) : rng.range(-15, 15));
        const int kind = rng.range(0, 9);
        if (kind < 6) {
            const std::uint8_t v = rng.u32() & 1;
            add_condition(ev, g, kConstantVt, &v, 1);
        } else if (kind < 8) {
            float s = rng.uniform(-0.05f, 0.1f);
            std::uint8_t f[8] = {};
            std::memcpy(f, &s, 4);
            f[4] = rng.u32() & 1;
            add_condition(ev, g, kTimerVt, f, 8);
        } else {
            const std::uint8_t f[8] = {0, static_cast<std::uint8_t>(rng.u32() & 1), 0, 1,
                                       static_cast<std::uint8_t>(rng.range(-3, 3)), 0, 0, 0};
            add_condition(ev, g, kGroupStateVt, f, 8);
        }
    }
    for (Group* g = ev->core.conditions.newest; g; g = g->older)
        for (Cond* c = g->conditions; c; c = c->next) {
            if (rng.chance(0.3)) c->result_flags ^= 1;
            if (rng.chance(0.2)) c->override_flags = static_cast<std::uint8_t>(rng.range(0, 3));
        }
    *ev->core.state = rng.chance(0.5) ? 0 : rng.u32();
    ev->core.conditions.current_instruction = rng.range(-2, 4);
    ev->core.conditions.remote_instruction = rng.range(-2, 4);
}

void test_holder(eboot_kit::Rng& rng) {
    g_scripted = false;
    int cases = 0;
    for (int round = 0; round < 3000; ++round) {
        reset_arena();
        place_game();  // the conditions are built by the game's code either way
        Ev* ev = new_event(rng);
        random_conditions(rng, ev);
        Holder* h = &ev->core.conditions;
        // The queries.
        for (int id = -17; id <= 17; ++id) {
            for (const std::int32_t raw : {id, static_cast<std::int32_t>(rng.u32() << 8) | (id & 0xff)}) {
                place_game();
                const auto q = reinterpret_cast<GUEST_ABI std::int32_t (*)(const Holder*, std::uint32_t)>(0x16ec5a0);
                const std::int32_t theirs = q(h, static_cast<std::uint32_t>(raw));
                const std::int32_t ours = SprjEmkConditionHolder_QueryGroup(h, static_cast<std::uint32_t>(raw));
                if (theirs != ours && ++g_bad <= 12) std::printf("QueryGroup(%d): the game's %d, ours %d\n", id, theirs, ours);
                ++cases;
            }
        }
        {
            const auto q = reinterpret_cast<GUEST_ABI std::int32_t (*)(const Holder*)>(0x16ec660);
            if (q(h) != SprjEmkConditionHolder_QueryMain(h) && ++g_bad <= 12) std::printf("QueryMain differs\n");
            for (const Group* g = h->newest; g; g = g->older) {
                const auto gq = reinterpret_cast<GUEST_ABI std::uint64_t (*)(const Group*)>(0x16ebcf0);
                const bool theirs = (gq(g) & 0xff) != 0;
                if (theirs != SprjEmkConditionGroup_Query(g) && ++g_bad <= 12) std::printf("Group::Query(%d) differs\n", g->group_id);
                ++cases;
            }
        }
        // The latch pass and the update, each from the same bytes.
        const bb::FD4Time time{0, rng.chance(0.1) ? rng.value(-1, 1) : rng.uniform(0, 0.05f)};
        for (const std::uint64_t fn : {std::uint64_t{0x16ec4a0}, std::uint64_t{0x16ec380}}) {
            snapshot();
            const std::uint32_t before = *ev->core.state;
            place_game();
            reinterpret_cast<HolderUpdateFn>(fn)(h, &time, ev);
            // The in-game compare's prediction, from the word before and the
            // conditions after, must be what the game's left.
            if (predicted_latch(h, before) != *ev->core.state && ++g_bad <= 12)
                std::printf("the compare's prediction for 0x%llx misses (round %d)\n", static_cast<unsigned long long>(fn), round);
            capture(g_theirs);
            restore();
            place_ours();
            reinterpret_cast<HolderUpdateFn>(fn)(h, &time, ev);
            capture(g_ours);
            same(fn == 0x16ec4a0 ? "LatchGroupBits" : "Holder::Update", round, 0);
            restore();
            ++cases;
        }
    }
    std::printf("%s: holder: %d queries, latch passes and updates over 3000 random holders\n", kTest, cases);
}

// ---- 3. The loop ------------------------------------------------------------

struct Op {
    std::int32_t bank, id;
    int weight;
};
const Op kOps[] = {{0, 1, 6},    {0, 0, 2},   {1, 0, 2},  {1000, 3, 2}, {1000, 4, 1},    {1000, 2, 1}, {2000, 3, 1}, {5, 0, 1},
                   {11, 0, 2},   {1014, 7, 1}, {7, 0, 1}, {2012, 1, 1}, {2012, 0, 1},    {2001, 0, 1}, {3, 0, 1}};

// An event with a script of random instructions (an EMEVD image of its own).
Ev* scripted_event(eboot_kit::Rng& rng) {
    Ev* ev = new_event(rng);
    const int n = rng.range(1, 24);
    auto* image = static_cast<std::uint8_t*>(heap_alloc(nullptr, 0x80 + 0x20 * n + 16 * n + 0x60, 16));
    std::memset(image, 0, 0x80);
    const std::int64_t table = 0x80, blob = 0x80 + 0x20 * n, repl = blob + 16 * n;
    std::memcpy(image + 0x28, &table, 8);
    std::memcpy(image + 0x78, &blob, 8);
    std::memcpy(image + 0x58, &repl, 8);
    int total = 0;
    for (const Op& op : kOps) total += op.weight;
    for (int i = 0; i < n; ++i) {
        int pick = rng.range(0, total - 1);
        const Op* op = kOps;
        while (pick >= op->weight) pick -= (op++)->weight;
        auto* in = reinterpret_cast<bb::SprjEmkInstructionRecord*>(image + table + 0x20 * i);
        std::memset(in, 0, 0x20);
        in->bank = op->bank;
        in->instruction_id = op->id;
        in->argument_size = 8;
        in->arguments_offset = 16 * i;
        std::uint8_t* a = image + blob + 16 * i;
        a[0] = static_cast<std::uint8_t>(rng.chance(0.4) ? 0 : rng.range(-4, 4));
        a[1] = static_cast<std::uint8_t>(rng.u32());
        a[2] = static_cast<std::uint8_t>(rng.range(-3, 3));
        a[3] = 0;
        a[4] = static_cast<std::uint8_t>(rng.u32() & 1);
    }
    // The event record: n instructions, maybe its own arguments and a
    // replacement of the first argument byte of some instruction.
    auto* rec = reinterpret_cast<bb::SprjEmkEventRecord*>(heap_alloc(nullptr, 0x30, 8));
    std::memset(rec, 0, 0x30);
    rec->event_id = ev->core.event_id;
    rec->instruction_count = static_cast<std::uint32_t>(n);
    rec->instructions_offset = 0;
    static const std::uint32_t kCodes[] = {0, 1, 2, 11, 21, 10};
    rec->behavior_code = kCodes[rng.range(0, 5)];
    rec->argument_replacements_offset = -1;
    if (rng.chance(0.4)) {
        ev->argument_size = 8;
        ev->arguments = static_cast<std::uint8_t*>(heap_alloc(nullptr, 8, 16));
        for (int i = 0; i < 8; ++i) ev->arguments[i] = static_cast<std::uint8_t>(rng.range(-4, 4));
        rec->argument_replacement_count = 1;
        rec->argument_replacements_offset = 0;
        std::uint8_t* r = image + repl;
        std::memset(r, 0, 0x20);
        const std::uint32_t which = static_cast<std::uint32_t>(rng.range(0, n - 1));
        const std::uint64_t dst = 0, src = static_cast<std::uint64_t>(rng.range(0, 7)), size = 1;
        std::memcpy(r, &which, 4);
        std::memcpy(r + 8, &dst, 8);
        std::memcpy(r + 0x10, &src, 8);
        std::memcpy(r + 0x18, &size, 4);
    }
    auto* rd = reinterpret_cast<bb::SprjEvdRuntimeData*>(heap_alloc(nullptr, 0x38, 8));
    std::memset(rd, 0, 0x38);
    rd->data = image;
    ev->runtime_data = rd;
    ev->event = rec;
    ev->instruction_index = 0;
    ev->instruction_delta = 0;
    ev->continue_update = 1;
    if (rng.chance(0.05)) ev->core.flags = 1;  // already retired
    // The first instruction, as the constructor takes it.
    std::uint8_t more = 1;
    reinterpret_cast<AdvanceFn>(kAdvance)(&ev->runtime_data, &more);
    return ev;
}

void test_loop(eboot_kit::Rng& rng) {
    g_scripted = true;
    event_flag_watch(true);
    int ticks = 0, ended = 0;
    for (int round = 0; round < 1500; ++round) {
        reset_arena();
        place_game();
        Ev* ev = scripted_event(rng);
        EventFlagChange drain[64];
        while (event_flag_take_changes(drain, 64, nullptr)) {
        }
        for (int tick = 0; tick < 40 && !(ev->core.flags & 1 && tick > 0); ++tick) {
            const bb::FD4Time time{0, rng.chance(0.05) ? rng.value(-1, 1) : 0.0333333351f};
            snapshot();
            const std::vector<std::uint8_t> flags_before(fx().flags, fx().flags + sizeof(fx().flags));
            place_game();
            reinterpret_cast<GUEST_ABI void (*)(Ev*, const bb::FD4Time*)>(0x16ecdc0)(ev, &time);
            capture(g_theirs);
            restore();
            place_ours();
            reinterpret_cast<GUEST_ABI void (*)(Ev*, const bb::FD4Time*)>(0x16ecdc0)(ev, &time);
            capture(g_ours);
            if (!same("Update", round, tick)) break;
            // Ours told the flag log of every completion flag it set.
            std::vector<std::uint32_t> told;
            std::size_t got;
            while ((got = event_flag_take_changes(drain, 64, nullptr)))
                for (std::size_t i = 0; i < got; ++i) told.push_back(drain[i].id);
            std::vector<std::uint32_t> flipped;
            for (std::uint32_t i = 0; i < sizeof(fx().flags) * 8; ++i) {
                const std::uint8_t m = static_cast<std::uint8_t>(0x80u >> (i & 7));
                if (!(flags_before[i >> 3] & m) && (fx().flags[i >> 3] & m)) flipped.push_back(i);
            }
            if (told != flipped && ++g_bad <= 12)
                std::printf("Update (%d, %d): %zu completion flags set, %zu told\n", round, tick, flipped.size(), told.size());
            ++ticks;
            // Between ticks: conditions flip, a partner moves on.
            for (Group* g = ev->core.conditions.newest; g; g = g->older)
                for (Cond* c = g->conditions; c; c = c->next) {
                    std::uint64_t vt;
                    std::memcpy(&vt, c, 8);
                    if (vt == kConstantVt && rng.chance(0.3)) reinterpret_cast<std::uint8_t*>(c)[0x28] ^= 1;
                }
            if (rng.chance(0.1)) ev->core.conditions.remote_instruction = rng.range(-1, 6);
        }
        ended += (ev->core.flags & 1) != 0;
    }
    event_flag_watch(false);
    std::printf("%s: loop: %d ticks of 1500 scripted events, %d of them ended\n", kTest, ticks, ended);
}

}  // namespace

int main() {
    eboot_kit::load(kTest);
    for (const Entry& e : kEntries) {
        if (std::memcmp(at(e.bn), e.bytes, e.len) != 0) {
            std::printf("%s: 0x%llx does not hold the entry the decomp expects\n", kTest, static_cast<unsigned long long>(e.bn));
            return 1;
        }
    }
    g_heap_vtable[0x58 / 8] = reinterpret_cast<void*>(&heap_alloc);
    g_heap_vtable[0x70 / 8] = reinterpret_cast<void*>(&heap_free);
    jump(0x247b720, reinterpret_cast<const void*>(&heap_of));
    jump(kBroadcast, reinterpret_cast<const void*>(&broadcast));
    jump(kEnqueueRestart, reinterpret_cast<const void*>(&enqueue_restart));
    jump(kDlPanic, reinterpret_cast<const void*>(&dl_panic));
    jump(kSetDrawEnable, reinterpret_cast<const void*>(&draw_stub));
    stub_banks(std::make_integer_sequence<int, static_cast<int>(std::size(kBanks))>{});
    reset_arena();
    set_globals();

    eboot_kit::Rng rng(0x45564d44);
    test_dispatch(rng);
    rng.specials = true;
    test_holder(rng);
    rng.specials = false;
    test_loop(rng);
    std::printf("%s: %d differ\n", kTest, g_bad);
    return g_bad ? 1 : 0;
}
