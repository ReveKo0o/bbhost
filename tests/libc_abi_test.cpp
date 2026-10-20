#include "hle/libc_abi.h"
#include "hle/libc_float.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    std::exit(1); } } while (0)

// Explicit SysV ABI also on Windows: guest ldiv returns quotient in RAX and
// remainder in RDX, not a packed pair of 32-bit Windows longs.
__attribute__((sysv_abi, noinline)) bb::libc::Div64 guest_div(std::int64_t n, std::int64_t d) {
    return bb::libc::divide(n, d);
}
__attribute__((sysv_abi, noinline)) double guest_log(double x, int selector) {
    return bb::libc::logarithm(x, selector);
}
__attribute__((sysv_abi, noinline)) float guest_logf(float x, int selector) {
    return bb::libc::logarithm(x, selector);
}

int main() {
    using namespace bb::libc;
    const std::int64_t numerators[] = {0, 0xdeadbeefLL, 0x123456789abcdefLL,
                                       -0x123456789abcdefLL, INT64_MAX, INT64_MIN};
    for (const auto n : numerators) for (const auto d : {127773LL, -127773LL}) {
        const auto v = guest_div(n, d);
        CHECK(v.quot == n / d && v.rem == n % d);
        CHECK(v.quot * d + v.rem == n);
    }
    // Dinkum selectors use EDI, independently of XMM0.
    CHECK(std::abs(guest_log(100.0, 1) - 2.0) < 1e-14);
    CHECK(std::abs(guest_log(8.0, -1) - 3.0) < 1e-14);
    CHECK(std::abs(guest_log(2.0, 0) - std::log(2.0)) < 1e-14);
    CHECK(guest_logf(100.0f, 4) == 2.0f);
    CHECK(guest_logf(8.0f, -4) == 3.0f);
    CHECK(std::isinf(guest_log(0.0, 0)) && guest_log(0.0, 0) < 0);
    CHECK(std::isnan(guest_log(-1.0, 1)));

    const std::uint64_t bit_patterns[] = {0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL,
        0x0010000000000000ULL, 0x3ff0000000000000ULL, 0x7ff0000000000000ULL,
        0xfff0000000000000ULL, 0x7ff8000000000000ULL, 0x7ff0000000000001ULL};
    const int classes[] = {0, 0, -2, -2, -1, -1, 1, 1, 2, 2};
    for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        double x;
        std::memcpy(&x, &bit_patterns[i], sizeof(x));
        CHECK(dtest(&x) == classes[i]);
    }

    // Match the eboot's 36-byte localtime_s destination followed by a cookie.
    struct GuardedTm { Tm value; unsigned char tail[20]; } g{};
    std::memset(g.tail, 0xa5, sizeof(g.tail));
    std::tm host{};
    host.tm_sec = 37; host.tm_min = 24; host.tm_hour = 19;
    host.tm_mday = 11; host.tm_mon = 6; host.tm_year = 124;
    host.tm_wday = 4; host.tm_yday = 192; host.tm_isdst = 1;
    tm_from_host(host, &g.value);
    for (auto byte : g.tail) CHECK(byte == 0xa5);
    const auto roundtrip = tm_to_host(&g.value);
    Tm again{};
    tm_from_host(roundtrip, &again);
    CHECK(std::memcmp(&again, &g.value, sizeof(Tm)) == 0);
    char date[32];
    CHECK(std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &roundtrip) == 19);
    CHECK(std::strcmp(date, "2024-07-11 19:24:37") == 0);

    CHECK(buffer_mode(0) == _IOFBF && buffer_mode(1) == _IOLBF && buffer_mode(2) == _IONBF);
    CHECK(buffer_mode(-1) == -1 && buffer_mode(3) == -1);
    FILE* stream = std::tmpfile();
    CHECK(stream && std::setvbuf(stream, nullptr, buffer_mode(2), 0) == 0);
    CHECK(std::fclose(stream) == 0);

    Lconv lc;
    const char* decimal = nullptr;
    std::memcpy(&decimal, reinterpret_cast<const char*>(&lc) + 0x48, sizeof(decimal));
    CHECK(std::strcmp(decimal, ".") == 0);
    CHECK(*lc.currency_symbol == 0 && *lc.thousands_sep == 0);
    for (auto flag : lc.monetary_flags) CHECK(flag == 127);
    CHECK(std::strcmp(set_c_locale(nullptr), "C") == 0);
    CHECK(std::strcmp(set_c_locale(""), "C") == 0);
    CHECK(std::strcmp(set_c_locale("C"), "C") == 0);
    CHECK(!set_c_locale("POSIX") && !set_c_locale("en_US.UTF-8"));

    struct Vector { const char* text; int base; std::int64_t value; std::size_t end; bool range; };
    const Vector vectors[] = {
        {"2147483648!", 10, 2147483648LL, 10, false},
        {"-9223372036854775808", 10, INT64_MIN, 20, false},
        {"9223372036854775808", 10, INT64_MAX, 19, true},
        {"-9223372036854775809", 10, INT64_MIN, 20, true},
        {"0xdeadBEEF!", 0, 0xdeadbeefLL, 10, false},
        {"ff!", 16, 255, 2, false}, {"0778", 0, 63, 3, false},
        {"1012", 2, 5, 3, false}, {"z!", 36, 35, 1, false},
        {" \t-42!", 10, -42, 5, false}, {"  nope", 10, 0, 0, false},
        {"0x", 16, 0, 0, false}, {"0xg", 0, 0, 0, false},
        {"0x1", 8, 0, 1, false}, {"42", 1, 0, 0, false},
        {"+-12", 10, INT64_MAX, 4, true}, {"--12", 10, INT64_MIN, 4, true}
    };
    for (const auto& v : vectors) {
        const auto n = parse_signed(v.text, v.base);
        CHECK(n.value == v.value && n.end - v.text == static_cast<std::ptrdiff_t>(v.end) && n.range_error == v.range);
        std::u16string wide;
        for (const char* p = v.text; *p; ++p) wide.push_back(static_cast<char16_t>(*p));
        const auto w = parse_signed(wide.c_str(), v.base);
        CHECK(w.value == v.value && w.end - wide.c_str() == static_cast<std::ptrdiff_t>(v.end) && w.range_error == v.range);
    }
    const auto max = parse_unsigned("18446744073709551615!", 10);
    CHECK(max.value == UINT64_MAX && *max.end == '!' && !max.range_error);
    const auto over = parse_unsigned("18446744073709551616!", 10);
    CHECK(over.value == UINT64_MAX && *over.end == '!' && over.range_error);
    const auto negative = parse_unsigned("-1", 10);
    CHECK(negative.value == UINT64_MAX && !negative.range_error);
    const std::u16string long_number = std::u16string(100, u'0') + u"2147483648!";
    const auto long_parse = parse_signed(long_number.c_str(), 10);
    CHECK(long_parse.value == 2147483648LL && *long_parse.end == u'!' && !long_parse.range_error);
    // U+0131 must not truncate to ASCII '1'.
    const char16_t nonascii[] = {u'2', 0x0131, u'3', 0};
    const auto stop = parse_signed(nonascii, 10);
    CHECK(stop.value == 2 && stop.end == nonascii + 1);
    const std::u16string long_float = std::u16string(100, u'0') + u"2.5!";
    char16_t* float_end;
    CHECK(wide_stod(long_float.c_str(), &float_end) == 2.5 && *float_end == u'!');
    const char16_t wide_digit[] = {u'2', 0x0131, u'3', 0};
    CHECK(wide_stod(wide_digit, &float_end) == 2 && float_end == wide_digit + 1);
    const char16_t wide_space[] = {0x0120, u'2', 0};
    CHECK(wide_stod(wide_space, &float_end) == 0 && float_end == wide_space);
    // Native _WStof also rounds this vector to 1.0. Host strtof would differ.
    CHECK(static_cast<float>(wide_stod(u"1.0000000596046448", nullptr)) == 1.0f);
    struct FloatRange { const char* text; bool range; };
    const FloatRange ranges[] = {{"1e999", true}, {"-1e999", true}, {"1e-999", true},
        {"0e999", false}, {"-0e-999", false}, {"inf", false}, {"nan", false},
        {"1e-40", false}, {"0x1p-2000", false}, {"0x1p1024", false}, {"nope", false}};
    for (const auto& v : ranges) {
        errno = 73;
        const auto d = parse_float<double>(v.text);
        const auto f = parse_float<float>(v.text);
        const std::u16string w(v.text, v.text + std::strlen(v.text));
        const auto wd = parse_wide_float<double>(w.c_str());
        const auto wf = parse_wide_float<float>(w.c_str());
        CHECK(d.range_error == v.range && f.range_error == v.range);
        CHECK(wd.range_error == v.range && wf.range_error == v.range && errno == 73);
        CHECK(d.end - v.text == wd.end - w.c_str());
        CHECK(f.end - v.text == wf.end - w.c_str());
    }
    CHECK(!parse_float<double>("1e-308").range_error);
    CHECK(parse_float<float>("1e-308").range_error);
    std::puts("libc ABI tests passed");
}
