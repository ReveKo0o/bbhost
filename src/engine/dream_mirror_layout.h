#pragma once

// The Hunter's Dream layout with the developers' mirror character made live.
//
// m21_00_00_00.msb places a part named ダミーCHR_再キャラメイク (a dummy
// character "for re-making the character") at the workshop's mirror: entity
// 2100202, talk script t210696 - the action "Put on Disguise", the one-time
// note "Wear clothing to change appearance." and then the game's appearance
// editor. The developers left it a DummyEnemy (part type 10), a cutscene
// placeholder that never spawns, so the talk never runs. As an Enemy (type
// 2) it spawns like the workbench and storage dummies beside it (c9010,
// invisible) and the mirror works.
//
// The game reads the parts list grouped by type, in file order (with the type
// changed in place, the Dream's load crashes), so the part moves rather than
// changes in place: its entry goes from slot 807 (the first
// DummyEnemy) to slot 742 (after the workbench dummy, in the Enemy group),
// the 65 entries between move down one, the type indices follow, and every
// field that names a part by its slot - an event's part (+0x48, +0x50,
// +0x5c), a part's collision and the like (+0x128, +0x13c, +0x144, +0x178,
// +0x180, +0x188) - is renumbered. Those fields are the ones the community
// mod "Restored Change Appearance Feature" changed when a map editor re-saved
// the file with the part moved; the result equals that re-save slot for slot,
// except that the part here is the developers' own (the mod put a copy of the
// workbench dummy there, sharing its entity id).
//
// The input is the 1.09 dump's file and the output a known file: both are
// checked by SHA-256, so nothing is ever guessed at.

#include <cstdint>
#include <string>
#include <vector>

// msb: the decompressed m21_00_00_00 MSB. True with it rewritten; false with
// the reason, msb untouched, when it is not the 1.09 file.
bool dream_mirror_layout(std::vector<std::uint8_t>& msb, std::string* why);
