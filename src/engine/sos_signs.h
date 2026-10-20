// How long a summon sign the player is answering stays in the game's list of
// sign entries before it gives up on it.
//
// The SOS sign manager's update (Binary Ninja 0x1c72360) ages each entry by
// the frame's seconds (its +0x4) and drops it - "the summon failed" message,
// the entry freed - once it is 180 s old, or 55 s old when it never got an
// answer (+0xc clear). The 180 is one float in .rodata (0x4d27a5c) read by
// that comparison alone. Waiting three minutes on a sign that is not coming
// keeps the player from touching another; online.sign_timeout_seconds
// replaces it (default 30, as the community patch "SosSignEntry Cooldown 30s"
// does; 0 keeps the game's 180).
#pragma once

struct ElfImage;

void sos_signs_install(ElfImage* image);
