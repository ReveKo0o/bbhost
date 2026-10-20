#include "host/text_entry.h"

namespace {

// The next code point of a UTF-8 string, or 0 at its end. A byte that does
// not start a well-formed sequence is skipped rather than read as one.
std::uint32_t next_code_point(const char*& p) {
    while (*p) {
        const auto b = static_cast<unsigned char>(*p);
        int extra = b < 0x80 ? 0 : (b & 0xe0) == 0xc0 ? 1 : (b & 0xf0) == 0xe0 ? 2 : (b & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0) {
            ++p;
            continue;
        }
        std::uint32_t cp = extra == 0 ? b : b & (0x3f >> extra);
        int i = 1;
        for (; i <= extra; ++i) {
            const auto c = static_cast<unsigned char>(p[i]);
            if ((c & 0xc0) != 0x80) break;
            cp = (cp << 6) | (c & 0x3f);
        }
        if (i <= extra) {
            ++p;
            continue;
        }
        p += extra + 1;
        return cp;
    }
    return 0;
}

void append_utf8(std::string& s, std::uint32_t cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        s.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

}  // namespace

std::uint32_t text_entry_accept(TextCharset cs, std::uint32_t cp) {
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0) || cp > 0x10ffff) return 0;
    if (cs == TextCharset::Any) return cp;
    // A Japanese or Chinese IME in its full-width mode types these for the
    // ASCII they stand for (U+FF01-U+FF5E, and the ideographic space).
    if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
    if (cp == 0x3000) cp = ' ';
    if (cp > 0x7e) return 0;
    if (cs == TextCharset::BasicLatin) return cp;
    // A glyph is eight of the 32 characters the game's own glyph editor steps
    // through (the table at 0x4b349a0): no 0, 1, l or o, which read alike.
    // The game and the server only ever see them lowercase.
    if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
    const bool digit = cp >= '2' && cp <= '9', letter = cp >= 'a' && cp <= 'z' && cp != 'l' && cp != 'o';
    return digit || letter ? cp : 0;
}

void TextEntry::begin(const char* initial_utf8, unsigned max, TextCharset cs, bool select_initial) {
    text.clear();
    max_chars = max ? max : 32;
    charset = cs;
    selected = false;
    insert(initial_utf8 ? initial_utf8 : "");
    selected = select_initial && !text.empty();
}

void TextEntry::insert(const char* utf8) {
    if (!utf8) return;
    for (const char* p = utf8; *p;) {
        const std::uint32_t cp = text_entry_accept(charset, next_code_point(p));
        if (!cp) continue;
        if (selected) {
            text.clear();
            selected = false;
        }
        if (length() >= max_chars) break;
        append_utf8(text, cp);
    }
}

void TextEntry::backspace() {
    if (selected) {
        erase_selection();
        return;
    }
    while (!text.empty()) {
        const auto b = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((b & 0xc0) != 0x80) break;  // the character's first byte: it is gone whole
    }
}

void TextEntry::erase_selection() {
    if (!selected) return;
    text.clear();
    selected = false;
}

unsigned TextEntry::length() const {
    unsigned n = 0;
    for (const char c : text) n += (static_cast<unsigned char>(c) & 0xc0) != 0x80;
    return n;
}
