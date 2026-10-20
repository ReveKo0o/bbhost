#pragma once

// Two invaders where the Chime Maidens allowed one.
//
// A host can be invaded while a maiden rings: her events put SpEffect 9020
// on the player, and the game then asks the sign server for invaders. In
// every story map and in the late areas she rings while no invader is in and
// stops at the first - instruction 3[29], "clients of type 1 (invaders)",
// == 0 to ring, >= 1 to stop - so those worlds hold four: the host, two
// cooperators, one invader. The chalice dungeons ring on while fewer than two
// are in (m29.emevd, 12907400), and hold five.
//
// This gives the late areas the chalices' rule, in their maiden events
// (xx04710 rings, xx04720 stops): == 0 becomes < 2 and >= 1 becomes >= 2.
//   m26 Nightmare of Mensis and Mergo's Loft (maidens 2600700, 2600701)
//   m33 Nightmare Frontier (3300700; its ring event also stops her itself)
//   m34 Hunter's Nightmare (3400790, 3400791)
//   m35 Research Hall (3500790, 3500791)
//   m36 Fishing Hamlet (3600790, 3600791)
// Two slips in the shipped scripts go too: the Hunter's Nightmare's second
// maiden rang as entity 3400701 (her other events say 3400791), and the
// Research Hall's second appeared on a Mensis flag (12604712 for 13504712).
//
// Each map's script must be the 1.09 dump's and comes out a known one: both
// are checked by SHA-256.

#include <cstdint>
#include <string>
#include <vector>

struct MaidenEventMap {
    const char* map;         // "m26_00_00_00"
    const char* sha_in;      // the decompressed .emevd as the dump has it
    const char* sha_out;     // and rewritten
};
// The maps this rewrites, in order.
const std::vector<MaidenEventMap>& maiden_event_maps();

// emevd: the decompressed event script of `map`. True with it rewritten;
// false with the reason, emevd untouched, when it is not the 1.09 script.
bool maiden_events_rewrite(const std::string& map, std::vector<std::uint8_t>& emevd, std::string* why);
