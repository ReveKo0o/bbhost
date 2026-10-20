#pragma once

// Button prompts that name the player's own keys, instead of the pad glyphs
// the disc was drawn for.
//
// How the game writes a prompt. It is a Scaleform image tag -
// `<img src='KG_R1' vspace='-8'>` - and the movie holds one image per pad
// button under those names. A tag reaches the screen two ways, and the port
// hooks both.
//
// Most of them the game builds at run time, and every one of the fifty-odd
// places that do call the same wide-string formatter, guest `sub_1ee7270`:
//
//     sub_1ee7270(&out, u"<img src='%s' vspace='-8'>", u"KG_R1")
//
// So the port hooks that one call: when the format is that one and the
// argument is a `KG_` tag, it swaps both for a format and a label of its own,
// and the game formats `[E]` where it would have placed a glyph. Nothing
// downstream changes - the string is still built, substituted and laid out by
// the game.
//
// The rest are in the message text itself: the world's tutorial notes carry
// the tag, and the key guide group carries named placeholders (`<?cancel?>`,
// `<?selectUD?>`; 117 names live in a table at 0x5754800). Those are rewritten
// at the message lookup instead, below.
//
// The tags, from the game's own help text and the pad layout in
// host/bindings.cpp: the `R_` set is the face buttons (R_U triangle, R_L
// square), the `L_` set the d-pad, `LS`/`RS` the sticks and `TP_L`/`TP_R` the
// touchpad's two sides. `OK` and `Cancel` are the odd pair: they are the
// Circle and Cross *images*, named the Japanese way round, and the game
// decides from the region which of them a confirm row gets - so the port reads
// the region too (engine/menu_pointer.h).
//
// The "Button prompts" setting (host/options.h) chooses: Auto follows the
// device the player last used (host_input_device, DS3's switch), and
// Controller leaves every glyph alone. A switch with a screen open shows at
// once: the key guide line is mended at the text field's SetText, so entries
// the screen formatted when it opened change too.
// `BBHOST_KEY_PROMPTS_TRACE=1` logs what each hook sees and self-checks the
// rewrite at startup.

#include <cstdint>

struct ElfImage;

// Hooks the formatter and the message lookup. The caller has already checked
// the eboot is 1.09.
void key_prompts_install(ElfImage* image);

// For the exit report: how much each hook rewrote, and tags seen that have no
// action.
void key_prompts_report();
