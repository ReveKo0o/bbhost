#include "engine/summon_invite.h"

#include "core/elf.h"
#include "core/thunk.h"
#include "engine/addr.h"
#include "log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

// Binary Ninja addresses (guest VA at the preferred slide), read this session.
constexpr std::uint64_t kFrpgNetMan = 0x593b120;        // FrpgNetMan* (singleton slot)
constexpr std::uint64_t kWrapperVTable = 0x5722570;     // SummonDataWrapper's vtable
constexpr std::uint64_t kMakeWrapper = 0x1886a10;       // (wrapper*, type u8, len u16, data*) -> bool
constexpr std::uint64_t kSignalingGate = 0x586cc09;     // u8, the NpMatching2 callbacks' gate
constexpr std::uint32_t kStepManagerOff = 0xc50;        // FrpgNetMan+0xc50: the SummonStepManager
constexpr std::uint32_t kPassiveListOff = 0x158;        // manager+0x158: list of MySosRequest entries
constexpr std::uint32_t kIdentityOff = 0xa68;           // FrpgNetMan+0xa68: our 16-byte identity
constexpr std::uint32_t kAreaOff = 0xa7c;               // FrpgNetMan+0xa7c: area id
constexpr std::uint32_t kLevelOff = 0xaa8;              // FrpgNetMan+0xaa8: soul level
constexpr std::uint32_t kPosOff = 0xaac;                // FrpgNetMan+0xaac: position, three floats
// The item's buffer, as sub_1886a10 lays it out: [0] unused, [1] type, [2..3]
// length, [4..] payload. handleIncomingItem (0x18ba340) reads the type-1
// payload at these buffer offsets.
constexpr std::size_t kPayloadSize = 0x44;

std::uint64_t g_slide = 0;
std::uint64_t g_size = 0;
bool g_enabled = true;
std::atomic<int> g_delivered{0};

std::uint64_t guest_of(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }
bool mapped(std::uint64_t va, std::size_t n) {
    return g_slide && va >= g_slide && va + n <= g_slide + g_size;
}
template <typename T>
T rd(std::uint64_t va) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), sizeof(T));
    return v;
}
// Heap pointers the game hands out are outside the image; they are readable
// in this process, so the check is only that they are not null or tiny.
bool plausible(std::uint64_t p) { return p > 0x10000; }

}  // namespace

void summon_invite_install(ElfImage* image) {
    g_slide = 0;
    if (!image || image->sha256 != kEboot109Sha256) return;
    // Off unless asked (2026-09-22): the host's game builds the type-1 item
    // itself - its sessionData is the session blob sub_1090b00 writes, not
    // the bare ConnectionArea - and sends it over its P2P socket once the
    // signaling connection to the guest is active (4.4). An item of ours
    // first would set the guest entry's sessionData and make the game's own
    // arrive as "already have" (reply 2). BBHOST_SUMMON_INVITE=1 keeps the
    // old delivery for experiments.
    g_enabled = false;
    if (const char* e = std::getenv("BBHOST_SUMMON_INVITE"); e && e[0] == '1') {
        g_enabled = true;
        host_log("summon invite: our own type-1 delivery on (BBHOST_SUMMON_INVITE=1)");
    }
    g_slide = image->mem.slide;
    g_size = image->mem.size;
}

void summon_invite_set_signaling_gate() {
    if (!g_slide) return;
    const std::uint64_t at = guest_of(kSignalingGate);
    if (!mapped(at, 1)) return;
    auto* p = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(at));
    if (*p != 1) {
        *p = 1;
        host_log("np: signaling gate set (data_586cc09 = 1)");
    }
}

void summon_invite_host_extra(json::Value& extra) {
    if (!g_slide) return;
    const std::uint64_t man = rd<std::uint64_t>(guest_of(kFrpgNetMan));
    if (!plausible(man)) return;
    extra.set("HostArea", static_cast<long long>(rd<std::uint32_t>(man + kAreaOff)));
    extra.set("HostLevel", static_cast<long long>(rd<std::uint32_t>(man + kLevelOff)));
    json::Value pos = json::Value::make_array();
    for (int i = 0; i < 3; ++i) pos.push(rd<float>(man + kPosOff + 4 * i));
    extra.set("HostPos", std::move(pos));
    extra.set("MemberTag", 1);
}

bool summon_invite_deliver(const SummonInvite& inv) {
    if (!g_slide || !g_enabled) {
        host_log("summon invite: room %llu not delivered (unbound or off)", static_cast<unsigned long long>(inv.room_id));
        return false;
    }
    const std::uint64_t man = rd<std::uint64_t>(guest_of(kFrpgNetMan));
    if (!plausible(man)) {
        host_log("summon invite: FrpgNetMan is not up yet");
        return false;
    }
    const std::uint64_t mgr = rd<std::uint64_t>(man + kStepManagerOff);
    if (!plausible(mgr)) {
        host_log("summon invite: no SummonStepManager");
        return false;
    }
    // The passive guest entries (createPassiveGuestEntry, sub_18b9180): a
    // std::list whose head node is at manager+0x158; node = {next, prev,
    // entry}. The entry's 8-byte sign session id is at +0x10, sessionReady at
    // +0xc8. Take the first one still waiting.
    const std::uint64_t head = rd<std::uint64_t>(mgr + kPassiveListOff);
    if (!plausible(head)) {
        host_log("summon invite: no passive guest list");
        return false;
    }
    std::uint64_t entry = 0;
    int seen = 0;
    for (std::uint64_t node = rd<std::uint64_t>(head); node && node != head && seen < 64; node = rd<std::uint64_t>(node), ++seen) {
        const std::uint64_t e = rd<std::uint64_t>(node + 0x10);
        if (!plausible(e)) continue;
        if (rd<std::uint32_t>(e + 0xc8) == 0) {
            entry = e;
            break;
        }
    }
    if (!entry) {
        host_log("summon invite: no waiting guest entry (%d seen) - has the bell been rung?", seen);
        return false;
    }
    // The payload: the entry's own id (what handleIncomingItem matches on),
    // the host's area and position, no session reference, and the 16-byte
    // ConnectionArea whose second qword mcp_Guest_JoinRoom reads as the room id.
    alignas(16) std::uint8_t payload[kPayloadSize];
    std::memset(payload, 0, sizeof(payload));
    std::memcpy(payload + 0x00, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(entry + 0x10)), 8);
    std::memcpy(payload + 0x0c, &inv.host_area, 4);
    std::memcpy(payload + 0x10, inv.host_pos, 12);
    std::memcpy(payload + 0x1c, &inv.host_level, 4);
    const std::int64_t no_session = static_cast<std::int64_t>(0x8000000000000000ull);
    std::memcpy(payload + 0x28, &no_session, 8);
    const std::uint32_t area_len = 16;
    std::memcpy(payload + 0x30, &area_len, 4);
    const std::uint32_t zero = 0;
    std::memcpy(payload + 0x34, &zero, 4);
    std::memcpy(payload + 0x38, &inv.member_tag, 4);
    std::memcpy(payload + 0x3c, &inv.room_id, 8);

    // The wrapper the game's own constructor fills: {vtable, buffer}.
    alignas(16) std::uint64_t wrapper[2] = {guest_of(kWrapperVTable), 0};
    const auto made = hle_call_guest<std::int64_t>(reinterpret_cast<void*>(static_cast<std::uintptr_t>(guest_of(kMakeWrapper))),
                                                   wrapper, 1, static_cast<std::int64_t>(kPayloadSize), payload);
    if ((made & 0xff) == 0 || !plausible(wrapper[1])) {
        host_log("summon invite: the game's wrapper constructor refused");
        return false;
    }
    // The sender as handleIncomingItem sees it: an object whose +0x20 points
    // at the sender's 16-byte identity (used for the block list and the log).
    char identity[16]{};
    std::snprintf(identity, sizeof(identity), "%s", inv.host_online_id.c_str());
    alignas(16) std::uint8_t sender[0x40];
    std::memset(sender, 0, sizeof(sender));
    const std::uint64_t idp = reinterpret_cast<std::uint64_t>(identity);
    std::memcpy(sender + 0x20, &idp, 8);
    // vtable slot 2 of the manager: handleIncomingItem.
    const std::uint64_t vtable = rd<std::uint64_t>(mgr);
    const std::uint64_t fn = plausible(vtable) ? rd<std::uint64_t>(vtable + 0x10) : 0;
    if (!fn) {
        host_log("summon invite: the manager has no vtable");
        return false;
    }
    hle_call_guest<std::int64_t>(reinterpret_cast<void*>(static_cast<std::uintptr_t>(fn)), mgr, sender, wrapper);
    const std::uint32_t ready = rd<std::uint32_t>(entry + 0xc8);
    host_log("summon invite: room %llu handed to the game as a type-1 item (sessionReady now %u, %d so far)",
             static_cast<unsigned long long>(inv.room_id), ready, g_delivered.fetch_add(1) + 1);
    // wrapper[1] (0x48 bytes from the game's allocator) is left to the
    // manager's own bookkeeping; a few dozen bytes an invite.
    return ready != 0;
}
