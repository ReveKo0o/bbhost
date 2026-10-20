#pragma once

// "Rebirth in the Nightmare" at the Altar of Despair.
//
// With the Yharnam Stone, the altar returns the hunter to their origin's level
// and attributes, gives back the blood echoes those levels cost, and opens the
// doll's level-up menu to spend them again; leaving it asks to accept or undo
// (closing that choice goes back to the menu). world.rebirth (BBHOST_REBIRTH=0|1,
// on by default): the altar's talk script is rewritten from the dump into
// <data>/bbhost/rebirth-assets at start (engine/rebirth_script.h) and served
// behind the player's own overlay; the host's half of its handshake runs once a
// frame. Off, the altar is the game's.

#include <string>

struct ElfImage;

void rebirth_install(ElfImage* image);
// Once a frame on the game's main thread (engine/frame_rate.cpp). Outside
// the altar's map it only reads where the hunter is.
void rebirth_tick();
// For the 300-flip report: the frames since the last call that read the
// altar's flags and that did not; empty when there were none.
std::string rebirth_report();
