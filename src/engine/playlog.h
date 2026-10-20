// How often the game sends its play log (the private server ingests it:
// positions, deaths, kills, boss clears for the site's map and profiles).
//
// SprjPlaylogSystem's tick (Binary Ninja 0x23f8640) counts a timer down every
// frame (+0xe0) and, at zero, compresses, encrypts and PUTs what it recorded
// (SprjPlaylogLoggerMan 0x23e5530), then reloads the timer from a float its
// static initializer sets (0x23f9ad0: `mov [0x5a9ecc0], 300.0f` at
// 0x23f9b86); the constructor (0x23f7950) starts the timer from the same
// float. 300 s is five minutes behind for a live map, and the last minutes of
// a session never upload. online.playlog_upload_seconds replaces that float
// in the initializer's immediate, before it runs; 0 keeps the game's 300.
// The same initializer stores the RegularLog (player position) period, 1.5f
// into 0x5a9eca8 at 0x23f9b68; online.playlog_sample_ms replaces it so the
// site's live map gets a point every 0.5 s to play back as walking.
//
// Measured against a local server (tools/map_validation.sh, 2026-10-01): at
// 15 s the uploads arrived every 15 s (plus one right after each death),
// 0.7-3.6 KB each; the PUT is a nonblocking sceHttp request, so it runs on
// hle/http.cpp's worker thread (18-28 ms) and the game's later status call
// found it done (0.0 ms wait) - the main loop does not wait on the network.
// Both deaths of the run (an enemy's and a fall) reached history.db.
#pragma once

struct ElfImage;

void playlog_install(ElfImage* image);
