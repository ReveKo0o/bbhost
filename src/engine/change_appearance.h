#pragma once

// The Hunter's Dream mirror: the developers' unfinished "Put on Disguise".
//
// The workshop's mirror has a talk character the developers placed and never
// enabled (engine/dream_mirror_layout.h): standing at it shows "Put on
// Disguise", the first time "Wear clothing to change appearance.", and opens
// the game's appearance editor - the character creation's Appearance list,
// in the world. world.change_appearance (BBHOST_CHANGE_APPEARANCE=0|1, on by
// default) makes the Dream's layout with that character live: the dump's
// m21_00_00_00.msb.dcx rewritten into <data>/bbhost/world-assets at start,
// again only when the generator changes, and served behind the player's own
// overlay (paths.mods) like the menu assets (engine/menu_assets.h). Off, the
// directory is not served and the Dream is the game's.
//
// Pointed at by the community mod "Restored Change Appearance Feature", which
// does the same with a re-saved layout of its own.

struct ElfImage;

void change_appearance_install(ElfImage* image);
// Once a frame on the game's main thread (engine/frame_rate.cpp): the
// editor's frame - character creation's ChrMake_BG window, with the model
// preview - opened, driven and closed around the Appearance list.
void change_appearance_tick();
