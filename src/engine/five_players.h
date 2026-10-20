#pragma once

// Five players in the late areas: the Nightmare of
// Mensis and Mergo's Loft, the Nightmare Frontier, and the Old Hunters'
// Hunter's Nightmare, Research Hall and Fishing Hamlet hold a host, two
// cooperators and two invaders, as the chalice dungeons do - their Chime
// Maidens ring on until two invaders are in (engine/maiden_events.h).
//
// world.five_players (BBHOST_FIVE_PLAYERS=0|1, on by default): the maps'
// event scripts are rewritten from the dump at start into
// <data>/bbhost/invasion-assets and served behind the player's own overlay.
// Every game in the session needs it: each runs the map's events, and a
// guest's shipped stop event ends the host's maiden at the first invader
// (its flag and SpEffect 9020 reach the host). Off, the maidens are the game's.

struct ElfImage;

void five_players_install(ElfImage* image);
