#include "guest_abi.h"
#include "hle/sysv_va.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    std::exit(1); } } while (0)

extern "C" GUEST_ABI int wide_body(char16_t* b, std::uint64_t n, const char16_t* fmt, SysvVaList* ap) {
    return sysv_vswprintf(b, n, fmt, ap);
}
SYSV_VA_ENTRY(wide_format, wide_body, 3, rcx)

int main() {
    std::array<char16_t, 300> b{};
    CHECK(wide_format(b.data(), b.size(), u"%ls%ls%ls", u"title:/Shader/", u"Tex1", u".vpo") == 22);
    CHECK(std::u16string(b.data()) == u"title:/Shader/Tex1.vpo");
    CHECK(wide_format(b.data(), b.size(), u"syntax error at line %d near: ", 137) == 31);
    CHECK(std::u16string(b.data()) == u"syntax error at line 137 near: ");
    CHECK(wide_format(b.data(), b.size(), u"\u3042[%*.*ls][%-4lc][%s][%c]", 5, 2, u"\u00e9\u3042\u03a9", 0x03a9, "ASCII", 'x') == 24);
    CHECK(std::u16string(b.data()) == u"\u3042[   \u00e9\u3042][\u03a9   ][ASCII][x]");
    CHECK(wide_format(b.data(), b.size(), u"%ld|%lld|%.2f|%d", 1234567890123LL, -9876543210LL, 1.25, -7) > 0);
    CHECK(std::u16string(b.data()) == u"1234567890123|-9876543210|1.25|-7");
    // More than three extra GP args spills to the guest caller's stack.
    CHECK(wide_format(b.data(), b.size(), u"%d %d %d %d %d %d %d %d %d %.1f", 1,2,3,4,5,6,7,8,9,2.5) == 21);
    CHECK(std::u16string(b.data()) == u"1 2 3 4 5 6 7 8 9 2.5");
    int count = -1;
    std::int64_t long_count = -1;
    CHECK(wide_format(b.data(), b.size(), u"\u3042%ls%n%ld%ln", u"\u00e9", &count, 1234567890123LL, &long_count) == 15);
    CHECK(count == 2 && long_count == 15);
    for (std::size_t n = 0; n < 7; ++n) {
        b.fill(0xa5a5);
        errno = 73;
        CHECK(wide_format(b.data(), n, u"abcd") == (n > 4 ? 4 : -1));
        CHECK(errno == 73);
        CHECK(b[n] == 0xa5a5);
        if (n) CHECK(b[std::min(n-1, std::size_t(4))] == 0);
        else CHECK(b[0] == 0xa5a5);
    }
    CHECK(wide_format(b.data(), b.size(), u"%ls", std::u16string(270, u'\u3042').c_str()) == 270);
    CHECK(std::u16string(b.data()).size() == 270);
    errno = 73;
    CHECK(wide_format(b.data(), b.size(), u"prefix%c", 0xe9) == -1 && errno == 73);
    CHECK(std::u16string(b.data()) == u"prefix");
    CHECK(wide_format(b.data(), b.size(), u"[%s][%3.1s]", "\xc3\xa9\xe3\x81\x82", "\xc3\xa9\xe3\x81\x82") == 9);
    CHECK(std::u16string(b.data()) == u"[\u00e9\u3042][  \u00e9]");
    errno = 0;
    CHECK(wide_format(b.data(), b.size(), u"prefix%s", "\xff") == -1 && errno == EILSEQ);
    CHECK(wide_format(b.data(), 0, u"") == 0);
    std::puts("UTF-16 SysV formatting tests passed");
}
