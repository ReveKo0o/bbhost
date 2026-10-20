// The guest's invite to a co-op room, delivered the way PSN's P2P channel
// would have: as a type-1 SummonData item handed to the game's own
// SummonStepManager. The game does the
// rest itself - sessionReady, startGuestJoinSession, sceNpMatching2JoinRoom.
#pragma once

#include "replay/json.h"

#include <cstdint>
#include <string>

struct ElfImage;

struct SummonInvite {
    std::uint64_t room_id = 0;
    std::uint32_t member_tag = 0;
    std::uint32_t host_area = 0;
    float host_pos[3] = {0, 0, 0};
    std::uint32_t host_level = 0;
    std::string host_online_id;
};

// Binds the 1.09 addresses; no-op (and every call below a no-op) otherwise.
void summon_invite_install(ElfImage* image);
// Must run on a thread with a guest TCB (the session dispatcher). Returns
// false, with a log line, when the game has no passive guest entry to give
// the item to (the player has not rung the bell) or the addresses are unbound.
bool summon_invite_deliver(const SummonInvite& inv);
// The host's side: area, position and level for the invite the server
// relays, read from FrpgNetMan. Adds HostArea/HostPos/HostLevel.
void summon_invite_host_extra(json::Value& extra);
// The NpMatching2 signaling gate (data_586cc09): the game's callbacks queue
// events only when it is 1; on PSN the library set it.
void summon_invite_set_signaling_gate();
