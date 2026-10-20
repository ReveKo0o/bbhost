// A test hook for online play: BBHOST_NP_TEST=guest[:delay] or
// host[:delay] dispatches, on the game's main thread once the world is up
// and `delay` seconds later, the Lua event the bells would have: the guest's
// sign (`OnEvent_SendSoulSign_NormalCoop`) or the host's search
// (`OnEvent_Call_SOS`), through the game's own LuaEvent_DispatchByName -
// the call the scripts' own event raisers make.
// Test scaffolding, hash-gated; off unless the variable is set.
#pragma once

struct ElfImage;

void np_test_install(ElfImage* image);
// Called every frame from the frame-time manager hook (the main thread).
void np_test_tick();

// BBHOST_GX_MAP_TRACE=1: who maps which GX buffer for writing (engine/gx_map_probe.cpp).
struct ElfImage;
