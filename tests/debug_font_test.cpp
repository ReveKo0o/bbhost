// plugins/debug_menu/debug_font.hpp: the debug font made from font14.bin is
// what the game's loader reads - a CCM glyph table of version 0x20000 whose
// regions lie on the TPF's DXT1 pages - and covers the characters the debug
// menu's own text uses; a glyph reads back as white with a black outline.
#include "debug_font.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

std::uint32_t rd16(const std::vector<std::uint8_t>& v, std::size_t at) { return v[at] | (v[at + 1] << 8); }
std::uint32_t rd32(const std::vector<std::uint8_t>& v, std::size_t at) { return rd16(v, at) | (rd16(v, at + 2) << 16); }

// A texel of a TPF page: 0 transparent, 1 black, 2 white (3 anything else).
int texel(const std::vector<std::uint8_t>& tpf, std::size_t dds, int x, int y) {
    const std::size_t block = dds + 128 + 8 * static_cast<std::size_t>((y / 4) * 128 + x / 4);
    const std::uint32_t c0 = rd16(tpf, block), c1 = rd16(tpf, block + 2), idx = rd32(tpf, block + 4);
    const int i = static_cast<int>((idx >> (2 * ((y % 4) * 4 + x % 4))) & 3);
    if (c0 > c1) return 3;  // never the four-colour mode
    if (i == 3) return 0;
    const std::uint32_t c = i == 0 ? c0 : i == 1 ? c1 : 0x1234;
    return c == 0 ? 1 : c == 0xffff ? 2 : 3;
}

}  // namespace

int main() {
    std::ifstream in(BB_FONT14_PATH, std::ios::binary);
    const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!blob.empty());
    debug_font::Files f;
    std::string err;
    CHECK(debug_font::build(blob.data(), blob.size(), &f, &err));
    if (g_fail) {
        std::printf("%s\n", err.c_str());
        return 1;
    }

    // The CCM header, as the original's.
    const std::vector<std::uint8_t>& ccm = f.ccm;
    CHECK(rd32(ccm, 0) == 0x20000);
    CHECK(rd32(ccm, 4) == ccm.size());
    CHECK(rd16(ccm, 0x0a) == 512 && rd16(ccm, 0x0c) == 512);
    const std::uint32_t regions = rd16(ccm, 0x0e), glyphs = rd32(ccm, 0x10), glyphs_at = rd32(ccm, 0x18);
    CHECK(rd32(ccm, 0x14) == 0x20 && glyphs_at == 0x20 + 8 * regions);
    CHECK(glyphs_at + 24 * glyphs == ccm.size());
    CHECK(ccm[0x1e] == f.pages && f.pages > 1);
    CHECK(static_cast<int>(glyphs) == f.glyphs);

    // The TPF: one 512x512 DXT1 page per texture, names in UTF-16.
    const std::vector<std::uint8_t>& tpf = f.tpf;
    CHECK(std::memcmp(tpf.data(), "TPF", 4) == 0);
    CHECK(rd32(tpf, 8) == static_cast<std::uint32_t>(f.pages));
    CHECK(tpf[0x0c] == 0 && tpf[0x0e] == 1);
    std::vector<std::size_t> pages;
    for (int i = 0; i < f.pages; ++i) {
        const std::size_t e = 0x10 + 20 * static_cast<std::size_t>(i);
        const std::size_t data = rd32(tpf, e), size = rd32(tpf, e + 4);
        CHECK(size == 128 + 512 * 512 / 2 && data + size <= tpf.size());
        CHECK(tpf[e + 8] == 1 && tpf[e + 10] == 1);
        CHECK(std::memcmp(tpf.data() + data, "DDS ", 4) == 0 && std::memcmp(tpf.data() + data + 84, "DXT1", 4) == 0);
        CHECK(rd32(tpf, data + 12) == 512 && rd32(tpf, data + 16) == 512);
        const std::size_t name = rd32(tpf, e + 12);
        char want[16];
        std::snprintf(want, sizeof(want), "DbgFont14h_%04d", i);
        for (int c = 0; c < 16; ++c) CHECK(rd16(tpf, name + 2 * static_cast<std::size_t>(c)) == static_cast<unsigned char>(want[c]));
        pages.push_back(data);
    }

    // Every glyph: sorted codes, its region on its page, its widths.
    std::map<std::uint32_t, std::size_t> by_code;
    std::uint32_t last = 0;
    for (std::uint32_t i = 0; i < glyphs; ++i) {
        const std::size_t e = glyphs_at + 24 * i;
        const std::uint32_t code = rd32(ccm, e), region = rd32(ccm, e + 4), page = rd16(ccm, e + 8);
        CHECK(i == 0 || code > last);
        last = code;
        CHECK(region >= 0x20 && region < glyphs_at && (region - 0x20) % 8 == 0);
        CHECK(page < static_cast<std::uint32_t>(f.pages));
        const std::uint32_t x1 = rd16(ccm, region), y1 = rd16(ccm, region + 2), x2 = rd16(ccm, region + 4), y2 = rd16(ccm, region + 6);
        CHECK(x1 < x2 && x2 <= 512 && y1 < y2 && y2 <= 512);
        CHECK(rd16(ccm, e + 12) == x2 - x1 && rd16(ccm, e + 14) + 1 == x2 - x1);
        by_code[code] = e;
    }
    // What the debug menu's text uses: ASCII, kana, kanji, the full-width
    // brackets and CP932's tilde and yen.
    for (std::uint32_t c = 0x20; c < 0x7f; ++c) CHECK(by_code.count(c));
    for (const char16_t c : std::u16string(u"全アクション有効パラメータ変更【】：～￥￢セーブモード")) CHECK(by_code.count(c));

    // 'A' on its page: white in the middle of its strokes, an outline around them.
    {
        const std::size_t e = by_code.at('A');
        const std::size_t region = rd32(ccm, e + 4);
        const int x1 = static_cast<int>(rd16(ccm, region)), y1 = static_cast<int>(rd16(ccm, region + 2));
        const int x2 = static_cast<int>(rd16(ccm, region + 4)), y2 = static_cast<int>(rd16(ccm, region + 6));
        const std::size_t dds = pages[rd16(ccm, e + 8)];
        int white = 0, black = 0, other = 0;
        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) {
                const int t = texel(tpf, dds, x, y);
                white += t == 2;
                black += t == 1;
                other += t == 3;
            }
        }
        CHECK(white > 10 && black > white && other == 0);
        // Its outline's edge never leaves the region.
        for (int y = y1; y < y2; ++y) CHECK(texel(tpf, dds, x1, y) != 2 && texel(tpf, dds, x2 - 1, y) != 2);
    }

    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("debug_font_test: ok - %d glyphs on %d pages, ccm %zu bytes, tpf %zu bytes\n", f.glyphs, f.pages, f.ccm.size(),
                f.tpf.size());
    return 0;
}
