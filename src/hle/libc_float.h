#pragma once

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace bb::libc {

template <typename Float, typename Char>
struct FloatParse {
    Float value;
    const Char* end;
    bool range_error;
};

// This preserves the existing host-based arithmetic, not native bit-exact
// rounding. Native decimal scaling (0x17a20/0x17d60) reports range only when
// a nonzero coefficient becomes zero/infinity, NOT for nonzero subnormals.
// Hex binary scaling (0x17790/0x17ba0) does not set errno at all.
template <typename Float>
inline FloatParse<Float, char> parse_float(const char* text) {
    if (!text) return {0, text, false};
    char* tail = nullptr;
    const int saved_errno = errno;
    const double parsed = std::strtod(text, &tail);
    const Float value = static_cast<Float>(parsed);
    errno = saved_errno;
    const char* p = text;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
    if (*p == '+' || *p == '-') ++p;
    const bool hex = p < tail && p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    bool nonzero_coefficient = false;
    if (!hex) {
        for (; p < tail && ((*p >= '0' && *p <= '9') || *p == '.'); ++p)
            nonzero_coefficient |= *p >= '1' && *p <= '9';
    }
    return {value, tail, nonzero_coefficient && (value == 0 || std::isinf(value))};
}

template <typename Float>
inline FloatParse<Float, char16_t> parse_wide_float(const char16_t* text) {
    if (!text) return {0, text, false};
    std::string narrow;
    for (const char16_t* p = text; *p; ++p) {
        // C-locale floating syntax is ASCII. A non-ASCII code unit must
        // remain a terminator, not alias an ASCII digit after truncation.
        narrow.push_back(*p < 128 ? static_cast<char>(*p) : '\x7f');
    }
    const auto parsed = parse_float<Float>(narrow.c_str());
    return {parsed.value, text + (parsed.end - narrow.c_str()), parsed.range_error};
}

inline double wide_stod(const char16_t* text, char16_t** end) {
    const auto parsed = parse_wide_float<double>(text);
    if (end) *end = const_cast<char16_t*>(parsed.end);
    return parsed.value;
}

}  // namespace bb::libc
