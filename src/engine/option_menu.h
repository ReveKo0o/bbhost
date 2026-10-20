#pragma once

// The PC port's options, as a section of the game's own System menu.
//
// System (the menu after Play Online / Play Offline) is a command list: six
// rows, each a caption plus a std::function, appended to a builder by
// `sub_1f4e2a0` and finalised by `sub_1fea630`. A row's function opens a
// section, and an opener is five instructions - it hands `sub_1f20900` a
// sprite name and a plain handler pointer, and the handler builds the rows.
// The engine owns the dialog, the cursor, the widgets, the key guide and the
// fade (option_menu.cpp has the full map).
//
// So the port writes its **own** opener and adds a **seventh row** that runs
// it. None of the game's five sections is redirected: Language still opens
// Language. Two guest patches carry it - a seventh slot in the section-name
// table, and the one call that finalises the System builder - and both are
// checked byte for byte. On by default; BBHOST_PC_OPTIONS=0 turns it off, and
// it stays off when the asset overlay does not supply the PCSetting sprite.

#include <cstdint>

struct ElfImage;

// Applies the guest patches. No-op when the switch is off, the eboot is not
// the known 1.09 build, or the overlay movie is missing.
void option_menu_install(ElfImage* image);

// Once a pump. The rows write their bytes whenever the player moves a cursor,
// so this is where a change becomes a setting: it seeds from the host's
// settings on the first call and pushes anything the player changed back.
// Cheap and safe when the patch is not installed.
void option_menu_poll();

// Every menu list update, on the game's menu thread (engine/menu_pointer.cpp):
// where the port changes what its own rows show.
void option_menu_list_update(void* comp, bool focused);

// Whether a click on item `index` of `list` means Circle wherever it lands on
// the row, rather than stepping the row's value (a key binding: choosing it
// starts the capture). On the menu thread, from the pointer.
bool option_menu_click_decides(void* list, int index);

// A yes/no question asked the way the options screens ask theirs (on the
// open PC Graphics screen's own popup stack, modal over it). Possible while
// PC Graphics is open; the question goes up on the next menu update.
// option_menu_confirm_take: 0 while unanswered, then once 1 (yes) or 2 (no,
// Circle, or the screen went away).
bool option_menu_confirm_possible();
void option_menu_confirm_ask(std::uint32_t message);
int option_menu_confirm_take();
// Answers an open question NO, as the player would (no close of its own).
void option_menu_confirm_close();
