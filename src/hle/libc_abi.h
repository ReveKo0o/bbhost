#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <limits>

namespace bb::libc {

// Dinkumware's layouts, verified against sce_module/libc.prx. Orbis uses
// LP64 integers, but its libc tm/lconv are NOT the FreeBSD/glibc structures.
struct Div64 {
    std::int64_t quot, rem;
};
static_assert(sizeof(Div64) == 16 && offsetof(Div64, rem) == 8);
inline Div64 divide(std::int64_t n, std::int64_t d) { return {n / d, n % d}; }

struct Tm {
    std::int32_t sec, min, hour, mday, mon, year, wday, yday, isdst;
};
static_assert(sizeof(Tm) == 36 && offsetof(Tm, isdst) == 32);
inline void tm_from_host(const std::tm& s, Tm* d) {
    *d = {s.tm_sec, s.tm_min, s.tm_hour, s.tm_mday, s.tm_mon,
          s.tm_year, s.tm_wday, s.tm_yday, s.tm_isdst};
}
inline std::tm tm_to_host(const Tm* g) {
    std::tm t{};
    t.tm_sec = g->sec;
    t.tm_min = g->min;
    t.tm_hour = g->hour;
    t.tm_mday = g->mday;
    t.tm_mon = g->mon;
    t.tm_year = g->year;
    t.tm_wday = g->wday;
    t.tm_yday = g->yday;
    t.tm_isdst = g->isdst;
    return t;
}

struct Lconv {
    const char* int_curr_symbol = "";
    const char* currency_symbol = "";
    const char* mon_decimal_point = "";
    const char* mon_thousands_sep = "";
    const char* mon_grouping = "";
    const char* positive_sign = "";
    const char* negative_sign = "";
    unsigned char monetary_flags[14] = {127, 127, 127, 127, 127, 127, 127,
                                       127, 127, 127, 127, 127, 127, 127};
    const char* decimal_point = ".";
    const char* thousands_sep = "";
    const char* grouping = "";
};
static_assert(sizeof(Lconv) == 96 && offsetof(Lconv, decimal_point) == 0x48);
inline char* set_c_locale(const char* name) {
    static char c[] = "C";
    return !name || !*name || (name[0] == 'C' && name[1] == '\0') ? c : nullptr;
}

// Guest buffering modes are 0/1/2; Microsoft's _IOLBF/_IONBF are 64/4.
inline int buffer_mode(int mode) {
    switch (mode) {
        case 0: return _IOFBF;
        case 1: return _IOLBF;
        case 2: return _IONBF;
        default: return -1;
    }
}

// _Dtest returns a short: normal=-1, denormal=-2, zero=0, infinity=1, NaN=2.
// Inspect bits instead of performing floating-point operations on sNaNs.
inline std::int16_t dtest(const double* p) {
    std::uint64_t bits;
    std::memcpy(&bits, p, sizeof(bits));
    bits &= 0x7fffffffffffffffull;
    const auto exponent = bits & 0x7ff0000000000000ull;
    if (exponent == 0x7ff0000000000000ull) return bits == exponent ? 1 : 2;
    if (!bits) return 0;
    return exponent ? -1 : -2;
}
template <typename Float> inline Float logarithm(Float x, int selector) {
    return selector > 0 ? std::log10(x) : selector < 0 ? std::log2(x) : std::log(x);
}

// The narrow and UTF-16 integer helpers share the same C-locale parser. No
// host long, locale, UTF-16 narrowing, arbitrary input cap, or host errno.
template <typename Char, typename Integer> struct IntegerParse {
    Integer value;
    const Char* end;
    bool range_error;
};
template <typename Char> inline const Char* skip_space(const Char* p) {
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
    return p;
}
template <typename Char> inline IntegerParse<Char, std::uint64_t> parse_unsigned(const Char* s, int base) {
    const Char* p = skip_space(s);
    const bool negative = *p == '-';
    if (*p == '+' || *p == '-') ++p;
    if (base < 0 || base == 1 || base > 36) return {0, s, false};
    const bool autodetect = base == 0;
    if (!base) base = *p == '0' ? 8 : 10;
    if ((autodetect || base == 16) && *p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    const Char* first = p;
    std::uint64_t value = 0;
    bool overflow = false;
    for (;;) {
        const auto c = static_cast<std::uint32_t>(*p);
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                               c >= 'a' && c <= 'z' ? c - 'a' + 10 :
                               c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 36;
        if (digit >= static_cast<unsigned>(base)) break;
        constexpr auto max = std::numeric_limits<std::uint64_t>::max();
        if (value > (max - digit) / static_cast<unsigned>(base)) overflow = true;
        if (!overflow) value = value * static_cast<unsigned>(base) + digit;
        ++p;
    }
    if (p == first) return {0, s, false};
    if (overflow) return {std::numeric_limits<std::uint64_t>::max(), p, true};
    return {negative ? 0 - value : value, p, false};
}
template <typename Char> inline IntegerParse<Char, std::int64_t> parse_signed(const Char* s, int base) {
    const Char* p = skip_space(s);
    const bool negative = *p == '-';
    if (*p == '+' || *p == '-') ++p;
    // Native signed helpers strip one sign then call the unsigned helper.
    const auto u = parse_unsigned(p, base);
    if (u.end == p) return {0, s, u.range_error};
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    const auto limit = static_cast<std::uint64_t>(max) + (negative ? 1 : 0);
    if (u.value > limit) return {negative ? min : max, u.end, true};
    const auto value = u.value == static_cast<std::uint64_t>(max) + 1 ? min :
                       negative ? -static_cast<std::int64_t>(u.value) : static_cast<std::int64_t>(u.value);
    return {value, u.end, u.range_error};
}

}  // namespace bb::libc
