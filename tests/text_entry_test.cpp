// host/text_entry.cpp: what the box over the game takes while the game's text
// dialogs are typed in - the name, a chalice glyph, the network password - and
// how it edits: the name's suggestion, the characters each box takes, the
// length, a paste.
#include "host/text_entry.h"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

// Typed into a box of `cs`, `max` long, from empty.
std::string typed(TextCharset cs, unsigned max, const char* text) {
    TextEntry e;
    e.begin("", max, cs, false);
    e.insert(text);
    return e.text;
}

}  // namespace

int main() {
    // The name: basic Latin, as the console's keyboard is for all three boxes.
    CHECK(typed(TextCharset::BasicLatin, 16, "Gehrman") == "Gehrman");
    CHECK(typed(TextCharset::BasicLatin, 16, "Lady Maria!") == "Lady Maria!");
    CHECK(typed(TextCharset::BasicLatin, 16, "Jos\xc3\xa9") == "Jos");             // an accented letter is not taken
    CHECK(typed(TextCharset::BasicLatin, 16, "\xe3\x81\x82" "a") == "a");          // nor kana an IME composed
    CHECK(typed(TextCharset::BasicLatin, 16, "\xef\xbc\xa1\xef\xbc\x91") == "A1"); // full-width A and 1 are A and 1
    CHECK(typed(TextCharset::BasicLatin, 16, "a\xe3\x80\x80" "b") == "a b");       // the ideographic space is a space
    CHECK(typed(TextCharset::BasicLatin, 16, "a\tb\r\n\x1b\x7f") == "ab");         // control characters never
    CHECK(typed(TextCharset::BasicLatin, 16, "\xff" "a\xc3") == "a");              // nor broken UTF-8
    // Sixteen characters, then nothing more.
    CHECK(typed(TextCharset::BasicLatin, 16, "abcdefghijklmnopqrst") == "abcdefghijklmnop");

    // The name's suggestion (player.name): selected, the first key replaces it.
    {
        TextEntry e;
        e.begin("Hunter", 16, TextCharset::BasicLatin, true);
        CHECK(e.selected && e.text == "Hunter");  // Enter on its own accepts it
        e.insert("G");
        CHECK(!e.selected && e.text == "G");
        e.insert("ehrman");
        CHECK(e.text == "Gehrman");
        // It no longer takes six of the sixteen characters away.
        e.begin("Hunter", 16, TextCharset::BasicLatin, true);
        e.insert("Abcdefghijklmnopqrst");
        CHECK(e.text == "Abcdefghijklmnop");
        // Backspace or Delete clears it whole; another Backspace has nothing left.
        e.begin("Hunter", 16, TextCharset::BasicLatin, true);
        e.backspace();
        CHECK(e.text.empty() && !e.selected);
        e.backspace();
        CHECK(e.text.empty());
        e.begin("Hunter", 16, TextCharset::BasicLatin, true);
        e.erase_selection();
        CHECK(e.text.empty());
        // A caret key keeps it, as text typed.
        e.begin("Hunter", 16, TextCharset::BasicLatin, true);
        e.keep();
        e.insert("s");
        CHECK(e.text == "Hunters");
        e.backspace();
        CHECK(e.text == "Hunter");
        e.erase_selection();  // nothing selected: Delete has nothing after the caret
        CHECK(e.text == "Hunter");
        // A paste the box takes nothing of leaves the suggestion as it was.
        e.begin("Hunter", 16, TextCharset::BasicLatin, true);
        e.insert("\xc3\xa9");
        CHECK(e.selected && e.text == "Hunter");
        // A starting text goes through the same rules, and an empty one has
        // nothing to select.
        e.begin("Jos\xc3\xa9", 16, TextCharset::BasicLatin, true);
        CHECK(e.text == "Jos");
        e.begin("", 16, TextCharset::BasicLatin, true);
        CHECK(!e.selected);
        // The game's own name, opened again: not a suggestion, typing adds to it.
        e.begin("Gehrman", 16, TextCharset::BasicLatin, false);
        e.insert("x");
        CHECK(e.text == "Gehrmanx");
    }

    // A chalice glyph: eight of 2-9 and a-z less l and o, lowercased.
    CHECK(typed(TextCharset::Glyph, 8, "kbmgfp2n") == "kbmgfp2n");
    CHECK(typed(TextCharset::Glyph, 8, "KBMG FP2N") == "kbmgfp2n");
    CHECK(typed(TextCharset::Glyph, 8, "lo01LO") == "");
    CHECK(typed(TextCharset::Glyph, 8, "8r6qxkmmz") == "8r6qxkmm");
    CHECK(typed(TextCharset::Glyph, 8, "\xef\xbd\x8b" "b") == "kb");  // full-width k
    CHECK(typed(TextCharset::Glyph, 8, "-_.,") == "");
    for (char c = '!'; c <= '~'; ++c) {
        const char s[2] = {c, 0};
        const std::string g = typed(TextCharset::Glyph, 8, s);
        const char want = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        const bool in = std::string("23456789abcdefghijkmnpqrstuvwxyz").find(want) != std::string::npos;
        CHECK(g == (in ? std::string(1, want) : std::string()));
    }

    // The port's own fields take anything printable; Backspace takes a whole
    // character, however many bytes it is.
    CHECK(typed(TextCharset::Any, 16, "J\xc3\xbcrgen") == "J\xc3\xbcrgen");
    {
        TextEntry e;
        e.begin("J\xc3\xbc", 16, TextCharset::Any, false);
        CHECK(e.length() == 2);
        e.backspace();
        CHECK(e.text == "J");
    }
    CHECK(typed(TextCharset::Any, 3, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9") == "\xc3\xa9\xc3\xa9\xc3\xa9");  // characters, not bytes
    CHECK(typed(TextCharset::Any, 16, "a\x1b\x7f\xc2\x85" "b") == "ab");
    CHECK(text_charset_ascii(TextCharset::BasicLatin) && text_charset_ascii(TextCharset::Glyph) &&
          !text_charset_ascii(TextCharset::Any));

    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("text_entry_test: ok\n");
    return 0;
}
