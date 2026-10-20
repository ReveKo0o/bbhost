#pragma once

// The game's menu steps: sub_201cce0 builds one refcounted object (the count
// is the int32 at +8) per menu it opens - a list, a dialog, the level-up
// screen - with the menu's type and name (u"ChrMakeCommandList", u"LevelUp").
// A feature that must know when a menu has gone holds a reference on its step:
// when the feature's is the only one left, the menu let go of it. One hook on
// the constructor serves every watch.

#include <cstdint>

struct ElfImage;

// A watch for steps of this type and name, from an install function; -1 when
// the table is full. The name must outlive the run (a literal).
int menu_steps_watch(std::uint64_t type, const char16_t* name);
// Hooks the constructor (once; later calls report the first's result). False
// on another eboot or unexpected bytes.
bool menu_steps_install(ElfImage* image);
// The step made under a watch since the last take, or 0. The hook runs before
// the constructor does: take it on the next frame, on the game's main thread.
std::uint64_t menu_steps_take(int watch);

// References, on the main thread.
inline std::int32_t* menu_step_count(std::uint64_t step) {
    return reinterpret_cast<std::int32_t*>(static_cast<std::uintptr_t>(step + 8));
}
void menu_step_hold(std::uint64_t step);
// Drops ours; at zero the step is destroyed through its vtable, as the game does.
void menu_step_release(std::uint64_t step);
