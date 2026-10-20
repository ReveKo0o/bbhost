// The guest va_list walker (hle/sysv_va.h) against the host's own printf and
// scanf, on Linux where the host va_list is the SysV one: every case goes
// through the asm shim (registers, xmm and stack arguments) and through a
// host va_list reinterpreted as SysvVaList, and both must match the host.
#include "guest_abi.h"
#include "hle/sysv_va.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" GUEST_ABI int test_snprintf_body(char* b, std::uint64_t n, const char* fmt, SysvVaList* ap) {
    return sysv_vsnprintf(b, n, fmt, ap);
}
extern "C" GUEST_ABI int test_sscanf_body(const char* s, const char* fmt, SysvVaList* ap) {
    return sysv_vsscanf(s, fmt, ap);
}
SYSV_VA_ENTRY(test_snprintf, test_snprintf_body, 3, rcx)
SYSV_VA_ENTRY(test_sscanf, test_sscanf_body, 2, rdx)

static int failures = 0;

// Through the host's va_list, reinterpreted (Linux: it is the SysV one; on
// Windows the host's is an MS-ABI pointer, so only the shim path runs).
static int via_host(char* b, std::size_t n, const char* fmt, ...) {
#if defined(_WIN32)
    (void)b;
    (void)n;
    (void)fmt;
    return -2;
#else
    va_list ap;
    va_start(ap, fmt);
    const int r = sysv_vsnprintf(b, n, fmt, reinterpret_cast<SysvVaList*>(ap));
    va_end(ap);
    return r;
#endif
}

// `wfmt` is the host's spelling of the guest format `fmt`: the same on
// Linux; on Windows a guest %ld (64-bit) is the host's %lld.
#define CHECK_FMT2(wfmt, fmt, ...)                                                                      \
    do {                                                                                                \
        char want[4096], got[4096], got2[4096];                                                         \
        const int wn = std::snprintf(want, sizeof(want), wfmt, __VA_ARGS__);                            \
        const int gn = test_snprintf(got, sizeof(got), fmt, __VA_ARGS__);                               \
        const int hn = via_host(got2, sizeof(got2), fmt, __VA_ARGS__);                                  \
        const bool host_ok = hn == -2 || (hn == gn && std::strcmp(got2, got) == 0);                     \
        if (wn != gn || std::strcmp(want, got) != 0 || !host_ok) {                                      \
            ++failures;                                                                                 \
            std::printf("FAIL %s\n  want %d '%s'\n  shim %d '%s'\n  host %d '%s'\n", fmt, wn, want, gn, \
                        got, hn, got2);                                                                 \
        }                                                                                               \
    } while (0)
#define CHECK_FMT(fmt, ...) CHECK_FMT2(fmt, fmt, __VA_ARGS__)

static void printf_cases() {
    CHECK_FMT("%d %i %u %x %X %o", -5, 7, 4000000000u, 0xbeef, 0xBEEF, 0777);
    CHECK_FMT2("%lld %lld %llu %llx %zu %jd %td", "%ld %lld %lu %llx %zu %jd %td", -123456789012LL, 9876543210LL,
               18446744073709551615ULL, 0xdeadbeefcafeULL, sizeof(int), static_cast<std::intmax_t>(-1),
               static_cast<std::ptrdiff_t>(-3));
    CHECK_FMT("%hhd %hd %hhu %hu %hhx", 300, 70000, -1, -1, 0x1ff);
    CHECK_FMT("[%5d] [%-5d] [%05d] [%+d] [% d] [%.3d] [%8.3d]", 42, 42, 42, 42, 42, 7, 7);
    CHECK_FMT("[%*d] [%-*d] [%.*d] [%*.*f]", 6, 42, 6, 42, 4, 42, 10, 2, 3.14159);
    CHECK_FMT("[%*d]", -6, 42);
    CHECK_FMT("%f %e %g %E %G %a %.0f %10.4f %-10.2f| %#.0f", 3.14159, 31415.9, 0.0001234, 2.5, 1e20, 1.0, 2.5,
              3.14159, 2.71828, 3.0);
    CHECK_FMT("%c%c%c [%3c] [%-3c]", 'a', 66, 0x141, 'x', 'y');
    CHECK_FMT("[%s] [%10s] [%-10s] [%.2s] [%10.3s] [%s]", "hello", "hi", "hi", "hello", "hello", "");
    CHECK_FMT("%d %f %d %f %d %f %d %f %d %f %d %f %d %f %d %f %d %f %d %f", 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5,
              6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5, 10, 10.5);
    CHECK_FMT("%s %s %s %s %s %s %s %s %s %d", "a", "b", "c", "d", "e", "f", "g", "h", "i", 99);
    CHECK_FMT("%f %f %f %f %f %f %f %f %f %f %f", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0);
    CHECK_FMT("%d %d %d %d %d %d %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    CHECK_FMT("100%% %d%%", 50);
    CHECK_FMT("%#x %#o %#X %#08x", 255, 8, 255, 255);
    CHECK_FMT("%Lf %Le", 1.5L, 2.5L);
    CHECK_FMT("%s", std::string(600, 'z').c_str());
    CHECK_FMT("%.500f", 1.0 / 3.0);
    CHECK_FMT2("%.3s|%5.1f|%-8lld|%08.3f|%+.2e", "%.3s|%5.1f|%-8ld|%08.3f|%+.2e", "abcdef", 2.25, 77LL, -3.14159,
               12345.678);
    {
        char want[64], got[64];
        void* p = reinterpret_cast<void*>(0x7ffc12345678ULL);
        std::snprintf(want, sizeof(want), "0x%llx [%14s] [%-14s]", 0x7ffc12345678ULL, "0x7ffc12345678",
                      "0x7ffc12345678");
        test_snprintf(got, sizeof(got), "%p [%14p] [%-14p]", p, p, p);
        if (std::strcmp(want, got) != 0) {
            ++failures;
            std::printf("FAIL %%p: want '%s' got '%s'\n", want, got);
        }
    }
    {
        int n1 = -1, n2 = -1;
        char got[64];
        test_snprintf(got, sizeof(got), "abc%ndef%n", &n1, &n2);
        if (n1 != 3 || n2 != 6 || std::strcmp(got, "abcdef") != 0) {
            ++failures;
            std::printf("FAIL %%n: %d %d '%s'\n", n1, n2, got);
        }
    }
    {
        char got[8];
        const int n = test_snprintf(got, sizeof(got), "%s", "truncated here");
        if (n != 14 || std::strcmp(got, "truncat") != 0) {
            ++failures;
            std::printf("FAIL truncation: %d '%s'\n", n, got);
        }
        if (test_snprintf(nullptr, 0, "%d%d", 12, 34) != 4) {
            ++failures;
            std::printf("FAIL measure\n");
        }
    }
    {
        const char16_t w[] = u"wideé";
        char got[32];
        test_snprintf(got, sizeof(got), "[%ls] [%.2ls] [%lc]", w, w, static_cast<int>(u'W'));
        if (std::strcmp(got, "[wide?] [wi] [W]") != 0) {
            ++failures;
            std::printf("FAIL %%ls: '%s'\n", got);
        }
    }
}

#define CHECK_SCAN2(input, wfmt, fmt, ...)                                                                       \
    do {                                                                                                         \
        const int wn = std::sscanf(input, wfmt, __VA_ARGS__);                                                    \
        std::string want = dump();                                                                               \
        reset();                                                                                                 \
        const int gn = test_sscanf(input, fmt, __VA_ARGS__);                                                     \
        std::string got = dump();                                                                                 \
        reset();                                                                                                 \
        if (wn != gn || want != got) {                                                                           \
            ++failures;                                                                                          \
            std::printf("FAIL scan '%s' with '%s'\n  want %d %s\n  got  %d %s\n", input, fmt, wn, want.c_str(), \
                        gn, got.c_str());                                                                        \
        }                                                                                                        \
    } while (0)
#define CHECK_SCAN(input, fmt, ...) CHECK_SCAN2(input, fmt, fmt, __VA_ARGS__)

static int i1, i2, i3, n1;
static long long l1;
static unsigned u1;
static float f1;
static double d1;
static char s1[32], s2[32], c1;
static short h1;
static signed char b1;
static void reset() {
    i1 = i2 = i3 = n1 = -77;
    l1 = -77;
    u1 = 77;
    f1 = -77;
    d1 = -77;
    std::memset(s1, 'Q', sizeof(s1));
    s1[31] = 0;
    std::memset(s2, 'Q', sizeof(s2));
    s2[31] = 0;
    c1 = 'Q';
    h1 = -77;
    b1 = -77;
}
static std::string dump() {
    char b[512];
    std::snprintf(b, sizeof(b), "i=%d,%d,%d n=%d l=%lld u=%u f=%g d=%g s='%s','%s' c=%d h=%d b=%d", i1, i2, i3, n1,
                  l1, u1, static_cast<double>(f1), d1, s1, s2, c1, h1, b1);
    return b;
}

static void scanf_cases() {
    reset();
    CHECK_SCAN("12 34 56", "%d %d %d", &i1, &i2, &i3);
    CHECK_SCAN("12,34", "%d,%d", &i1, &i2);
    CHECK_SCAN("12 34", "%d,%d", &i1, &i2);
    CHECK_SCAN("  -7 0x1f 077", "%d %i %i", &i1, &i2, &i3);
    CHECK_SCAN("hello world", "%s %s", s1, s2);
    CHECK_SCAN("hello world", "%3s%n", s1, &n1);
    CHECK_SCAN("skip 42 keep", "%*s %d %s", &i1, s1);
    CHECK_SCAN("deadbeef", "%x", &u1);
    CHECK_SCAN("3.5 2.25", "%f %lf", &f1, &d1);
    CHECK_SCAN("-1234567890123", "%lld", &l1);
    CHECK_SCAN2("-1234567890123", "%lld", "%ld", &l1);
    CHECK_SCAN("abc123", "%[a-z]%d", s1, &i1);
    CHECK_SCAN("abc]123", "%[]a-z]%d", s1, &i1);
    CHECK_SCAN("xyz 9", "%[^ ] %d", s1, &i1);
    CHECK_SCAN("q7", "%c%d", &c1, &i1);
    CHECK_SCAN("ab", "%c%c", &c1, s1);
    CHECK_SCAN("300 70000", "%hhd %hd", &b1, &h1);
    CHECK_SCAN("", "%d", &i1);
    CHECK_SCAN("   ", "%d", &i1);
    CHECK_SCAN("x", "%d", &i1);
    CHECK_SCAN("5 x", "%d %d", &i1, &i2);
    CHECK_SCAN("50%", "%d%%", &i1);
    CHECK_SCAN("50%7", "%d%%%d", &i1, &i2);
    CHECK_SCAN("key = value", "%s = %s", s1, s2);
    CHECK_SCAN("1 2 3 4 5 6 7 8", "%d %d %d %d %hd %hhd %u %lld", &i1, &i2, &i3, &n1, &h1, &b1, &u1, &l1);
    CHECK_SCAN("m24_01", "m%d_%d", &i1, &i2);
    CHECK_SCAN("v1.09", "v%d.%d%n", &i1, &i2, &n1);
}

int main() {
    printf_cases();
    scanf_cases();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::puts("sysv_va_test ok");
    return 0;
}
