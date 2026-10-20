// The event-script interpreter (EMEVD) as our source (docs/decomp.md): the
// loop that runs every map's event scripts and the condition logic it waits
// on, in the game's place as leaves (decomp/decomp.h).
//
// Each running event is a SprjEmkEventIns (0xe0 bytes; the SDK's layout,
// include/bbhost/engine/sprj/emk_system.hpp), updated once a frame by
// SprjEmkSystem::Update through its vtable:
//
//   SprjEmkEventIns::Update (0x16ecdc0)
//     the event's conditions updated and latched (Holder::Update); then, as
//     long as the main condition group (group 0) is satisfied: group 0 latched
//     and cleared, one instruction dispatched, the next one taken (Advance,
//     the game's), until an instruction makes the event wait, yield or end.
//   emevd_dispatch_instruction (0x1bb93a0)
//     an instruction to its bank's function by bank: 0-13, 1000-1014 and
//     2000-2013 through three jump tables; 1014 (labels) does nothing and
//     says so; 2012 id 1 is handled inline; anything else returns 0, which
//     the loop ignores - an unknown instruction is skipped.
//   SprjEmkEventIns::EndOrRestart (0x16ed100, the vtable's +0x30)
//     the end of an event (and its restart record, when the event's script
//     says it restarts), or its restart; either way the event's completion
//     flag, event id + slot, when that is 99 or more.
//
// Conditions sit in signed-byte groups (SprjEmkConditionHolder at event
// +0x30). A condition's result is its override's value when the override is
// on (+0x18 bit 0, value bit 1), else its own result (+0x10 bit 0). A group
// with a negative id is satisfied when any of its conditions is (OR), any
// other when all are (AND); an empty one always is. Group 0 is the one the
// event waits on; groups 1..15 and -1..-15 are condition registers latched
// into the event's state word, bit (id > 0 ? id : 16 - id) - 1, once
// satisfied - only while the event's own progress (+0x48) is ahead of its
// partners' (+0x4c), and never cleared here. A query of group 0 while the
// event is not ahead of its partners answers 2: "already done elsewhere".
//
//   SprjEmkConditionHolder::Update (0x16ec380), LatchGroupBits (0x16ec4a0),
//   QueryGroup (0x16ec5a0), QueryMain (0x16ec660), SprjEmkConditionGroup::Query
//   (0x16ebcf0).
//
// Everything these call stays the game's and is called at its address, so a
// function switched off (BBHOST_DECOMP_OFF) is the game's for its callers here
// too: the groups' and conditions' own updates, Advance, Holder::Clear, the
// bank functions, the restart queue, the event-sync packet (0x31) and
// DL_PANIC.
//
// The completion flag is written as the game writes it: straight into the flag
// store - an inlined copy of SetEventFlag(id, 1), which does nothing besides
// that write - and the change is passed to the flag log and plugins'
// on_event_flag (decomp/events/flag_store.h), which until now never saw an
// event end.
#include "decomp/decomp.h"
#include "decomp/events/flag_store.h"
#include "decomp/guest.h"

#include "bbhost/engine/fd4.hpp"
#include "bbhost/engine/sprj/emk_system.hpp"
#include "bbhost/engine/sprj/event_resource.hpp"
#include "core/thunk.h"
#include "log.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace decomp;

namespace {

using Ev = bb::SprjEmkEventIns;
using Holder = bb::SprjEmkConditionHolder;
using Group = bb::SprjEmkConditionGroup;
using Cond = bb::SprjEmkConditionCore;

static_assert(offsetof(Ev, core) + offsetof(bb::SprjEmkEventCore, conditions) == 0x30, "the holder at event +0x30");
static_assert(offsetof(Holder, current_instruction) == 0x18 && offsetof(Holder, remote_instruction) == 0x1c,
              "the holder's progress words");
static_assert(offsetof(Ev, runtime_data) == 0x80 && offsetof(Ev, instruction_index) == 0xa0 &&
                  offsetof(Ev, instruction) == 0xb0 && offsetof(Ev, instruction_delta) == 0xc0 &&
                  offsetof(Ev, suppress_callback_once) == 0xc8,
              "the interpreter's fields");
static_assert(offsetof(Group, group_id) == 8 && offsetof(Group, conditions) == 0x10 && offsetof(Group, older) == 0x18 &&
                  offsetof(Group, newer) == 0x20,
              "a condition group");
static_assert(offsetof(Cond, next) == 8 && offsetof(Cond, result_flags) == 0x10 && offsetof(Cond, override_flags) == 0x18,
              "a condition's prefix");
static_assert(offsetof(bb::SprjEmkSystem, dispatcher) == 0x38 && offsetof(bb::SprjEvdRuntimeData, data) == 8,
              "the dispatcher, a script's image");

// The game's functions these call, at their Binary Ninja addresses.
constexpr std::uint64_t kHolderUpdate = 0x16ec380;
constexpr std::uint64_t kLatch = 0x16ec4a0;
constexpr std::uint64_t kHolderClear = 0x16ebff0;
constexpr std::uint64_t kBroadcast = 0x16ee540;  // event-sync packet 0x31, type 1, to every session member
constexpr std::uint64_t kDispatch = 0x1bb93a0;
constexpr std::uint64_t kAdvance = 0x16ef2e0;
constexpr std::uint64_t kEnqueueRestart = 0x16f1930;
constexpr std::uint64_t kMemcpy = 0x2a1a1b0;
constexpr std::uint64_t kDlPanic = 0x24b55b0;
constexpr std::uint64_t kSetDrawEnable = 0x17ca580;  // bank 2012 id 1
// Globals: the singleton slots and the default heap.
constexpr std::uint64_t kEmkSystemSlot = 0x593b0c0;
constexpr std::uint64_t kEventFlagManSlot = 0x593b100;
constexpr std::uint64_t kDefaultHeapSlot = 0x5940420;
// DL_PANIC's arguments at the singletons' null checks: the source file, its
// format and line, and the class name.
constexpr std::uint64_t kPanicFile = 0x4d3b369, kPanicFormat = 0x4d3b3bd, kSystemName = 0x4d3117d, kFlagManName = 0x4d3b19a;
constexpr int kPanicLine = 0xb1;

// The bank functions, by bank: 0-13, 1000-1014, 2000-2013 (0: none).
constexpr std::uint64_t kLow[14] = {0x1bc7160, 0x1bc8630, 0, 0x1bbd1d0, 0x1bb9c50, 0x1bc40d0, 0, 0, 0, 0, 0, 0x1bc2f60, 0, 0};
constexpr std::uint64_t kMid[15] = {0x1bc75b0, 0x1bc8a90, 0, 0x1bbedd0, 0, 0x1bc43f0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr std::uint64_t kHigh[14] = {0x1bc8030, 0, 0x1bc53b0, 0x1bc0420, 0x1bba6a0, 0x1bc46e0, 0x1bc64e0,
                                     0x1bc37a0, 0x1bb97d0, 0x1bc5d90, 0x1bc6ad0, 0x1bc31e0, 0, 0x16eb3b0};
constexpr std::int32_t kLabelBank = 1014, kDrawBank = 2012;

template <class T>
T* slot(std::uint64_t bn) {
    return *reinterpret_cast<T**>(static_cast<std::uintptr_t>(game_address(bn)));
}

using HolderUpdateFn = GUEST_ABI void (*)(Holder*, const bb::FD4Time*, Ev*);
using LatchFn = GUEST_ABI void (*)(Holder*, void*, Ev*);
using ClearFn = GUEST_ABI void (*)(Holder*);
using EventFn = GUEST_ABI void (*)(Ev*);
using DispatchFn = GUEST_ABI u8 (*)(void*, Ev*, float);
using AdvanceFn = GUEST_ABI u8 (*)(void*, u8*);
using EndFn = GUEST_ABI void (*)(Ev*, std::uint32_t);
using DigitFn = GUEST_ABI std::int32_t (*)(Ev*, std::uint32_t);
using GroupUpdateFn = GUEST_ABI void (*)(Group*, const bb::FD4Time*, Ev*);
using EnqueueFn = GUEST_ABI void (*)(void*, void*);
using MemcpyFn = GUEST_ABI void* (*)(void*, const void*, std::uint64_t);
using AllocFn = GUEST_ABI void* (*)(void*, std::uint64_t, std::uint64_t);
using PanicFn = GUEST_ABI void (*)(const char*, int, const char*, ...);
using DrawFn = GUEST_ABI void (*)(std::int32_t, std::uint32_t);

// A virtual call through the event's own vtable (+0x30 end/restart, +0x40
// its script's behaviour digit).
template <class F>
F virt(const void* object, std::size_t at) {
    const auto* vt = *reinterpret_cast<const std::uint8_t* const*>(object);
    return *reinterpret_cast<const F*>(vt + at);
}

void panic(std::uint64_t name) {
    game_function<PanicFn>(kDlPanic)(reinterpret_cast<const char*>(game_address(kPanicFile)), kPanicLine,
                            reinterpret_cast<const char*>(game_address(kPanicFormat)),
                            reinterpret_cast<const char*>(game_address(name)));
}

inline bool effective(const Cond* c) {
    return (c->override_flags & 1) ? (c->override_flags & 2) != 0 : (c->result_flags & 1) != 0;
}

// A group's conditions: any (negative id) or all; empty, satisfied.
inline bool satisfied(const Group* g) {
    const Cond* c = g->conditions;
    if (g->group_id < 0) {
        if (!c) return true;
        for (; c; c = c->next)
            if (effective(c)) return true;
        return false;
    }
    for (; c; c = c->next)
        if (!effective(c)) return false;
    return true;
}

// The state-word bit a group latches into. Its index is taken modulo 32, as
// the game's bt and shl take it.
inline std::uint32_t latch_bit(std::int8_t id) {
    const int k = (id > 0 ? id : 16 - id) - 1;
    return 1u << (k & 31);
}

// The main group: 1 satisfied (or none), 0 not; 2 when the event is not ahead
// of its partners.
inline std::int32_t main_state(const Holder* h) {
    const std::int32_t r = h->main_group ? (satisfied(h->main_group) ? 1 : 0) : 1;
    return h->current_instruction > h->remote_instruction ? r : 2;
}

// The latch pass: Holder::Update's second half and LatchGroupBits whole.
inline void latch(const Holder* h, Ev* ev) {
    for (const Group* g = h->oldest; g; g = g->newer) {
        if (h->current_instruction <= h->remote_instruction) continue;
        const std::int8_t id = g->group_id;
        if (id == 0) continue;
        if (*ev->core.state & latch_bit(id)) continue;
        if (satisfied(g)) *ev->core.state |= latch_bit(id);
    }
}

// For the exit report: instructions dispatched and those no bank function
// takes; events ended and restarted, completion flags newly set.
std::atomic<std::uint64_t> g_dispatched{0}, g_unknown{0}, g_ended{0}, g_restarted{0}, g_completed{0};

}  // namespace

// ---- In the game's place --------------------------------------------------

// 0x1bb93a0: (dispatcher, event, the frame's dt). The bank function's own
// result (its al), or 1 for a label or bank 2012 id 1, else 0.
DECOMP_LEAF u8 emevd_dispatch_instruction(void* d, Ev* ev, float dt) {
    const bb::SprjEmkInstructionRecord* in = ev->instruction;
    const std::int32_t bank = in->bank;
    g_dispatched.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t fn = 0;
    if (bank < 1000) {
        if (static_cast<std::uint32_t>(bank) <= 13) fn = kLow[bank];
    } else if (bank < 2000) {
        if (bank == kLabelBank) return 1;
        if (static_cast<std::uint32_t>(bank - 1000) <= 14) fn = kMid[bank - 1000];
    } else if (static_cast<std::uint32_t>(bank - 2000) <= 13) {
        if (bank == kDrawBank) {
            if (in->instruction_id != 1) {
                g_unknown.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            // Its arguments: the event's substituted copy, else the script's own.
            const u8* args = ev->instruction_arguments;
            if (!args) {
                const u8* image = ev->instruction_source->data;
                std::int64_t blob;
                std::memcpy(&blob, image + 0x78, 8);
                args = image + blob + in->arguments_offset;
            }
            std::int32_t id;
            std::memcpy(&id, args, 4);
            game_function<DrawFn>(kSetDrawEnable)(id, args[4] != 0);
            return 1;
        }
        fn = kHigh[bank - 2000];
    }
    if (!fn) {
        g_unknown.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return game_function<DispatchFn>(fn)(d, ev, dt);
}

// 0x16ec380: every group's update (its conditions'), oldest first, then the
// latch pass.
DECOMP_LEAF void SprjEmkConditionHolder_Update(Holder* h, const bb::FD4Time* time, Ev* ev) {
    for (Group* g = h->oldest; g; g = g->newer) virt<GroupUpdateFn>(g, 0)(g, time, ev);
    latch(h, ev);
}

// 0x16ec4a0: the latch pass alone. Its second argument is not read.
DECOMP_LEAF void SprjEmkConditionHolder_LatchGroupBits(Holder* h, void*, Ev* ev) { latch(h, ev); }

// 0x16ec5a0: group `id`'s state, the newest group of that id; a group that
// does not exist is satisfied. Group 0 answers 2 while the event is not ahead.
// The id is the low byte of its register, as the game reads it.
DECOMP_LEAF std::int32_t SprjEmkConditionHolder_QueryGroup(const Holder* h, std::uint32_t id_reg) {
    const auto id = static_cast<std::int8_t>(id_reg);  // the game's callers set sil alone
    std::int32_t r = 1;
    for (const Group* g = h->newest; g; g = g->older) {
        if (g->group_id != id) continue;
        r = satisfied(g) ? 1 : 0;
        break;
    }
    if (id != 0) return r;
    return h->current_instruction > h->remote_instruction ? r : 2;
}

// 0x16ec660
DECOMP_LEAF std::int32_t SprjEmkConditionHolder_QueryMain(const Holder* h) { return main_state(h); }

// 0x16ebcf0
DECOMP_LEAF bool SprjEmkConditionGroup_Query(const Group* g) { return satisfied(g); }

// 0x16ecdc0: the interpreter loop (above). `time` is the frame's FD4Time.
DECOMP_LEAF void SprjEmkEventIns_Update(Ev* ev, const bb::FD4Time* time) {
    Holder* h = &ev->core.conditions;
    game_function<HolderUpdateFn>(kHolderUpdate)(h, time, ev);
    for (;;) {
        const std::int32_t r = main_state(h);
        if (r <= 0) return;
        // Group 0 done with: latched (and broadcast) when satisfied here, then
        // cleared with every other group.
        for (const Group* g = h->newest; g; g = g->older) {
            if (g->group_id != 0) continue;
            if (g->conditions) {
                if (r == 1) {
                    game_function<LatchFn>(kLatch)(h, nullptr, ev);
                    if (!ev->suppress_callback_once) {
                        ev->core.network_source = 10000;
                        game_function<EventFn>(kBroadcast)(ev);
                    } else {
                        ev->suppress_callback_once = 0;
                    }
                }
                game_function<ClearFn>(kHolderClear)(h);
            }
            break;
        }
        if (!ev->instruction_source || !ev->instruction) {
            virt<EndFn>(ev, 0x30)(ev, 0);
            return;
        }
        auto* system = slot<std::uint8_t>(kEmkSystemSlot);
        if (!system) {
            panic(kSystemName);
            system = slot<std::uint8_t>(kEmkSystemSlot);
            if (!system) return;  // the game's would read through null
        }
        void* d = *reinterpret_cast<void**>(system + offsetof(bb::SprjEmkSystem, dispatcher));
        if (!d) return;
        game_function<DispatchFn>(kDispatch)(d, ev, time->time);
        u8 more = 1;
        const u8 ok = game_function<AdvanceFn>(kAdvance)(&ev->runtime_data, &more);
        h->current_instruction = ev->instruction_index;
        if (!ok) {
            virt<EndFn>(ev, 0x30)(ev, 0);
            return;
        }
        if (!more) return;
    }
}

namespace {

// The flag store read as the game reads it.
struct Direct {
    template <typename T>
    bool operator()(std::uint64_t at, T* out) const {
        *out = *reinterpret_cast<const T*>(static_cast<std::uintptr_t>(at));
        return true;
    }
};

}  // namespace

// 0x16ed100: end the event (restart's low byte 0) or restart it.
DECOMP_LEAF void SprjEmkEventIns_EndOrRestart(Ev* ev, std::uint32_t restart) {
    ev->core.conditions.remote_instruction = 0;
    ((restart & 0xff) ? g_restarted : g_ended).fetch_add(1, std::memory_order_relaxed);
    if (restart & 0xff) {
        *ev->core.state = 0;
        game_function<ClearFn>(kHolderClear)(&ev->core.conditions);
        const std::int32_t index = ev->instruction_index;
        if (index > 0) {
            ev->instruction_delta = -index;
            ev->continue_update = 0;
        }
    } else {
        // An event whose script restarts it (behaviour digit 0 is 1) leaves a
        // record: id, slot, map, a copy of its arguments, its script.
        if (!(ev->core.flags & 1) && virt<DigitFn>(ev, 0x40)(ev, 0) == 1) {
            void* heap = slot<void>(kDefaultHeapSlot);
            auto* rec = static_cast<std::uint8_t*>(virt<AllocFn>(heap, 0x58)(heap, 0x28, 8));
            std::memset(rec + 0x10, 0, 8);
            std::memset(rec + 0x18, 0, 4);
            std::memset(rec + 0x20, 0, 8);
            const std::int32_t slot_i = ev->core.slot;
            std::memcpy(rec, &ev->core.event_id, 4);
            std::memcpy(rec + 4, &slot_i, 4);
            std::memcpy(rec + 8, &ev->core.map_id, 4);
            const std::uint32_t size = ev->argument_size;
            const std::uint8_t* args = ev->arguments;
            if (args && size) {
                std::memcpy(rec + 0x18, &size, 4);
                void* heap2 = slot<void>(kDefaultHeapSlot);
                void* copy = virt<AllocFn>(heap2, 0x58)(heap2, size, 0x10);
                std::memcpy(rec + 0x10, &copy, 8);
                game_function<MemcpyFn>(kMemcpy)(copy, args, size);
            }
            std::memcpy(rec + 0x20, &ev->runtime_data, 8);
            void* system = slot<void>(kEmkSystemSlot);
            if (!system) {
                panic(kSystemName);
                system = slot<void>(kEmkSystemSlot);
            }
            game_function<EnqueueFn>(kEnqueueRestart)(system, rec);
        }
        const std::uint32_t count = ev->event ? ev->event->instruction_count : 0;
        ev->instruction_delta = static_cast<std::int32_t>(count - static_cast<std::uint32_t>(ev->instruction_index));
        ev->core.flags = static_cast<std::uint16_t>(ev->core.flags | 1);
    }
    // The completion flag.
    const std::int32_t id = ev->core.event_id, slot_i = ev->core.slot;
    if ((id | slot_i) >= 0) {
        const std::int32_t flag = static_cast<std::int32_t>(static_cast<std::uint32_t>(slot_i) + static_cast<std::uint32_t>(id));
        if (flag >= 99) {
            auto* man = slot<std::uint8_t>(kEventFlagManSlot);
            if (!man) {
                panic(kFlagManName);
                man = slot<std::uint8_t>(kEventFlagManSlot);
            }
            std::uint64_t at = 0;
            std::uint8_t mask = 0;
            // Where the game's would divide by a zero block size or read through
            // a null manager, ours writes nothing.
            if (man && sprj_event_flag::locate(reinterpret_cast<std::uint64_t>(man), static_cast<std::uint32_t>(flag), Direct{}, &at,
                                               &mask)) {
                auto* byte = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(at));
                const std::uint8_t was = *byte;
                *byte = static_cast<std::uint8_t>(was | mask);
                if (!(was & mask)) {
                    g_completed.fetch_add(1, std::memory_order_relaxed);
                    event_flag_changed(static_cast<std::uint32_t>(flag), true);
                }
            }
        }
    }
    ev->core.network_source = 10000;
}

// ---- Compare runs ----------------------------------------------------------
//
// The three queries only read: both run and agree. LatchGroupBits and
// Holder::Update only ever set bits of the state word, so ours predicts the
// word from the conditions as they stand after the game's call (the latch is
// the last thing either does), and the game's must have left exactly that.
// EndOrRestart's effects are predicted from the event before the game's runs
// - its fields, the restart record it queues, the completion flag - and
// checked after. The loop and the dispatcher have no compare: they call bank
// functions that act on the world, which cannot run twice; tests/decomp/event_interpreter_test.cpp
// runs them against the game's on synthetic events.

namespace {

void* g_game_dispatch = nullptr;
void* g_game_update = nullptr;
void* g_game_holder_update = nullptr;
void* g_game_latch = nullptr;
void* g_game_query_group = nullptr;
void* g_game_query_main = nullptr;
void* g_game_group_query = nullptr;
void* g_game_end = nullptr;
DecompCompare g_cmp_holder_update, g_cmp_latch, g_cmp_query_group, g_cmp_query_main, g_cmp_group_query, g_cmp_end;

void count(DecompCompare& c, bool same, const char* fn, const Ev* ev, ull theirs, ull ours) {
    c.calls.fetch_add(1, std::memory_order_relaxed);
    if (same) return;
    if (c.differ.fetch_add(1, std::memory_order_relaxed) < 8) {
        host_log("decomp: %s of event %d slot %d differs: the game's 0x%llx, ours 0x%llx", fn, ev ? ev->core.event_id : -1,
                 ev ? ev->core.slot : -1, theirs, ours);
    }
}

// The bits ours latches into `state` for this holder's groups as they stand.
std::uint32_t predicted_latch(const Holder* h, std::uint32_t state) {
    for (const Group* g = h->oldest; g; g = g->newer) {
        if (h->current_instruction <= h->remote_instruction || g->group_id == 0) continue;
        if (!(state & latch_bit(g->group_id)) && satisfied(g)) state |= latch_bit(g->group_id);
    }
    return state;
}

GUEST_ABI void compare_holder_update(Holder* h, const bb::FD4Time* time, Ev* ev) {
    const std::uint32_t before = *ev->core.state;
    hle_call_guest<std::int64_t>(g_game_holder_update, h, time, ev);
    // The conditions as the game's latch pass saw them: its last step.
    const std::uint32_t theirs = *ev->core.state, ours = predicted_latch(h, before);
    count(g_cmp_holder_update, ours == theirs, "SprjEmkConditionHolder::Update", ev, theirs, ours);
}

GUEST_ABI void compare_latch(Holder* h, void* unused, Ev* ev) {
    const std::uint32_t ours = predicted_latch(h, *ev->core.state);
    hle_call_guest<std::int64_t>(g_game_latch, h, unused, ev);
    const std::uint32_t theirs = *ev->core.state;
    count(g_cmp_latch, ours == theirs, "SprjEmkConditionHolder::LatchGroupBits", ev, theirs, ours);
}

GUEST_ABI std::int32_t compare_query_group(const Holder* h, std::uint32_t id) {
    const std::int32_t ours = SprjEmkConditionHolder_QueryGroup(h, id);
    const auto theirs = static_cast<std::int32_t>(hle_call_guest<std::int64_t>(g_game_query_group, h, id));
    count(g_cmp_query_group, ours == theirs, "SprjEmkConditionHolder::QueryGroup", nullptr, static_cast<ull>(theirs),
          static_cast<ull>(ours));
    return theirs;
}

GUEST_ABI std::int32_t compare_query_main(const Holder* h) {
    const std::int32_t ours = SprjEmkConditionHolder_QueryMain(h);
    const auto theirs = static_cast<std::int32_t>(hle_call_guest<std::int64_t>(g_game_query_main, h));
    count(g_cmp_query_main, ours == theirs, "SprjEmkConditionHolder::QueryMain", nullptr, static_cast<ull>(theirs),
          static_cast<ull>(ours));
    return theirs;
}

GUEST_ABI bool compare_group_query(const Group* g) {
    const bool ours = SprjEmkConditionGroup_Query(g);
    const bool theirs = (hle_call_guest<std::int64_t>(g_game_group_query, g) & 0xff) != 0;
    count(g_cmp_group_query, ours == theirs, "SprjEmkConditionGroup::Query", nullptr, theirs, ours);
    return theirs;
}

// The restart records in the system's queue, newest last (the SDK's list).
std::size_t restart_records(const void* system, void** out, std::size_t n) {
    if (!system) return 0;
    const auto* q = reinterpret_cast<const bb::SprjEmkRestartQueue*>(static_cast<const std::uint8_t*>(system) +
                                                                     offsetof(bb::SprjEmkSystem, restart_queue));
    std::size_t k = 0;
    for (const bb::SprjEmkRestartNode* node = q->sentinel ? q->sentinel->next : nullptr; node && node != q->sentinel && k < n;
         node = node->next)
        out[k++] = node->record;
    return k;
}

GUEST_ABI void compare_end(Ev* ev, std::uint32_t restart) {
    // Predicted from the event as it is now.
    const bool again = (restart & 0xff) != 0;
    const std::int32_t index = ev->instruction_index;
    std::int32_t delta = ev->instruction_delta;
    std::uint8_t cont = ev->continue_update;
    std::uint16_t flags = ev->core.flags;
    bool record = false;
    if (again) {
        if (index > 0) {
            delta = -index;
            cont = 0;
        }
    } else {
        record = !(flags & 1) &&
                 static_cast<std::int32_t>(hle_call_guest<std::int64_t>(reinterpret_cast<void*>(virt<DigitFn>(ev, 0x40)), ev, 0)) == 1;
        const std::uint32_t n = ev->event ? ev->event->instruction_count : 0;
        delta = static_cast<std::int32_t>(n - static_cast<std::uint32_t>(index));
        flags = static_cast<std::uint16_t>(flags | 1);
    }
    std::uint64_t flag_at = 0;
    std::uint8_t flag_mask = 0, flag_was = 0;
    const std::int32_t id = ev->core.event_id, slot_i = ev->core.slot;
    const std::int32_t flag = static_cast<std::int32_t>(static_cast<std::uint32_t>(slot_i) + static_cast<std::uint32_t>(id));
    auto* man = slot<std::uint8_t>(kEventFlagManSlot);
    const bool flagged = (id | slot_i) >= 0 && flag >= 99 && man &&
                         sprj_event_flag::locate(reinterpret_cast<std::uint64_t>(man), static_cast<std::uint32_t>(flag), Direct{},
                                                 &flag_at, &flag_mask);
    if (flagged) flag_was = *reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(flag_at));
    void* system = slot<void>(kEmkSystemSlot);
    void* before[64];
    const std::size_t had = restart_records(system, before, 64);

    hle_call_guest<std::int64_t>(g_game_end, ev, restart);

    bool same = ev->core.conditions.remote_instruction == 0 && ev->core.network_source == 10000 &&
                ev->instruction_delta == delta && ev->continue_update == cont && ev->core.flags == flags;
    if (again) {
        same = same && *ev->core.state == 0 && !ev->core.conditions.newest && !ev->core.conditions.oldest &&
               !ev->core.conditions.main_group;
    }
    if (flagged) {
        const std::uint8_t now = *reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(flag_at));
        same = same && (now & flag_mask);
        if (!(flag_was & flag_mask) && (now & flag_mask)) event_flag_changed(static_cast<std::uint32_t>(flag), true);
    }
    // The record: one new in the queue when predicted, its fields the event's.
    void* after[65];
    const std::size_t has = restart_records(system, after, 65);
    if (has <= 64 && had < 64) {
        std::size_t fresh = 0;
        const std::uint8_t* rec = nullptr;
        for (std::size_t i = 0; i < has; ++i) {
            bool old = false;
            for (std::size_t j = 0; j < had && !old; ++j) old = after[i] == before[j];
            if (!old) {
                ++fresh;
                rec = static_cast<const std::uint8_t*>(after[i]);
            }
        }
        same = same && fresh == (record ? 1u : 0u);
        if (record && rec) {
            const auto* r = reinterpret_cast<const bb::SprjEmkRestartRecord*>(rec);
            same = same && r->event_id == id && r->slot == slot_i && r->map_id == ev->core.map_id &&
                   r->runtime_data == ev->runtime_data;
            const bool args = ev->arguments && ev->argument_size;
            same = same && r->argument_size == (args ? ev->argument_size : 0) &&
                   (args ? r->arguments && std::memcmp(r->arguments, ev->arguments, ev->argument_size) == 0 : !r->arguments);
        }
    }
    count(g_cmp_end, same, again ? "SprjEmkEventIns::EndOrRestart (restart)" : "SprjEmkEventIns::EndOrRestart (end)", ev,
          static_cast<ull>(ev->instruction_delta), static_cast<ull>(delta));
}

// The entries' first whole instructions, as the eboot has them.
constexpr u8 kDispatchEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8b, 0x86, 0xb0, 0x00, 0x00, 0x00, 0x8b, 0x08};
constexpr u8 kUpdateEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                               0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xf6};
constexpr u8 kHolderUpdateEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54,
                                     0x53, 0x49, 0x89, 0xd6, 0x49, 0x89, 0xf4, 0x49, 0x89, 0xff};
// mov rsi, [rdi+8]; jmp short; a 10-byte nop.
constexpr u8 kLatchEntry[] = {0x48, 0x8b, 0x77, 0x08, 0xeb, 0x0e, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
// mov rcx, rdi; a 13-byte nop.
constexpr u8 kQueryGroupEntry[] = {0x48, 0x89, 0xf9, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
// mov rax, [rdi+0x10]; test; je; mov rcx, [rax+0x10]; cmp byte [rax+8], 0.
constexpr u8 kQueryMainEntry[] = {0x48, 0x8b, 0x47, 0x10, 0x48, 0x85, 0xc0, 0x74, 0x6e,
                                  0x48, 0x8b, 0x48, 0x10, 0x80, 0x78, 0x08, 0x00};
// mov rcx, [rdi+0x10]; cmp byte [rdi+8], 0; js; mov al, 1; jmp short; nop.
constexpr u8 kGroupQueryEntry[] = {0x48, 0x8b, 0x4f, 0x10, 0x80, 0x7f, 0x08, 0x00, 0x78, 0x2d, 0xb0, 0x01, 0xeb, 0x06, 0x66, 0x90};
constexpr u8 kEndEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                            0x55, 0x41, 0x54, 0x53, 0x50, 0x48, 0x89, 0xfb};

void report() {
    host_log("decomp: event scripts: %llu instructions dispatched, %llu the game has no function for (skipped, as the game skips "
             "them)",
             static_cast<ull>(g_dispatched.load()), static_cast<ull>(g_unknown.load()));
}

void report_end() {
    host_log("decomp: event scripts: %llu events ended, %llu restarted; %llu completion flags set and passed to the flag log",
             static_cast<ull>(g_ended.load()), static_cast<ull>(g_restarted.load()), static_cast<ull>(g_completed.load()));
}

}  // namespace

void decomp_event_interpreter_add() {
    const char* area = "events";
    DecompFunction dispatch{"emevd_dispatch_instruction", area, 0x1bb93a0, kDispatchEntry, sizeof(kDispatchEntry),
                            reinterpret_cast<void*>(&emevd_dispatch_instruction), DecompKind::Leaf, &g_game_dispatch};
    dispatch.report = &report;
    decomp_add(dispatch);
    decomp_add({"SprjEmkEventIns::Update", area, 0x16ecdc0, kUpdateEntry, sizeof(kUpdateEntry),
                reinterpret_cast<void*>(&SprjEmkEventIns_Update), DecompKind::Leaf, &g_game_update});
    DecompFunction end{"SprjEmkEventIns::EndOrRestart", area, 0x16ed100, kEndEntry, sizeof(kEndEntry),
                       reinterpret_cast<void*>(&SprjEmkEventIns_EndOrRestart), DecompKind::Leaf, &g_game_end,
                       reinterpret_cast<void*>(&compare_end), &g_cmp_end};
    end.report = &report_end;
    decomp_add(end);
    decomp_add({"SprjEmkConditionHolder::Update", area, 0x16ec380, kHolderUpdateEntry, sizeof(kHolderUpdateEntry),
                reinterpret_cast<void*>(&SprjEmkConditionHolder_Update), DecompKind::Leaf, &g_game_holder_update,
                reinterpret_cast<void*>(&compare_holder_update), &g_cmp_holder_update});
    decomp_add({"SprjEmkConditionHolder::LatchGroupBits", area, 0x16ec4a0, kLatchEntry, sizeof(kLatchEntry),
                reinterpret_cast<void*>(&SprjEmkConditionHolder_LatchGroupBits), DecompKind::Leaf, &g_game_latch,
                reinterpret_cast<void*>(&compare_latch), &g_cmp_latch});
    decomp_add({"SprjEmkConditionHolder::QueryGroup", area, 0x16ec5a0, kQueryGroupEntry, sizeof(kQueryGroupEntry),
                reinterpret_cast<void*>(&SprjEmkConditionHolder_QueryGroup), DecompKind::Leaf, &g_game_query_group,
                reinterpret_cast<void*>(&compare_query_group), &g_cmp_query_group});
    decomp_add({"SprjEmkConditionHolder::QueryMain", area, 0x16ec660, kQueryMainEntry, sizeof(kQueryMainEntry),
                reinterpret_cast<void*>(&SprjEmkConditionHolder_QueryMain), DecompKind::Leaf, &g_game_query_main,
                reinterpret_cast<void*>(&compare_query_main), &g_cmp_query_main});
    decomp_add({"SprjEmkConditionGroup::Query", area, 0x16ebcf0, kGroupQueryEntry, sizeof(kGroupQueryEntry),
                reinterpret_cast<void*>(&SprjEmkConditionGroup_Query), DecompKind::Leaf, &g_game_group_query,
                reinterpret_cast<void*>(&compare_group_query), &g_cmp_group_query});
}
