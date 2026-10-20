// Optional differential oracle. No game bytes are embedded: BBHOST_LIBC_ELF
// must name the user's decrypted sce_module/libc.elf.
#include "hle/libc_abi.h"
#include "hle/libc_stdio.h"
#include "hle/libc_float.h"
#include "guest_abi.h"
#include "hle/sysv_va.h"

#include <elf.h>
#include <sys/mman.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
    std::exit(1); } } while (0)

extern "C" GUEST_ABI int oracle_wide_body(char16_t* b, std::uint64_t n, const char16_t* fmt, SysvVaList* ap) {
    return sysv_vswprintf(b, n, fmt, ap);
}
SYSV_VA_ENTRY(oracle_wide_format, oracle_wide_body, 3, rcx)

namespace {
int oracle_errno;
std::uint64_t oracle_guard = 0x123456789abcdefULL;
int* oracle_error() { return &oracle_errno; }
void* oracle_tls(const std::uint64_t* descriptor) {
    // Only the audited native strerror TLS descriptor is supported.
    CHECK(descriptor[1] == 0x478);
    static thread_local char error_buffer[22];
    return error_buffer;
}
std::int64_t oracle_seek_result = 0x100000123LL;
std::int64_t oracle_seek(int fd, std::int64_t offset, int whence) {
    CHECK(fd == 7 && offset == 0 && whence == SEEK_CUR);
    return oracle_seek_result;
}
void unresolved_import() {
    std::fprintf(stderr, "native libc called an import outside this oracle's scope\n");
    std::abort();
}

struct NativeLibc {
    std::vector<char> file;
    unsigned char* base = nullptr;
    std::size_t size = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> tags;
    std::unordered_map<std::string, std::uint64_t> exports;
    std::size_t dynlib = 0;

    template <typename T> T get(std::size_t offset) const {
        CHECK(offset <= file.size() && sizeof(T) <= file.size() - offset);
        T result;
        std::memcpy(&result, file.data() + offset, sizeof(T));
        return result;
    }
    std::string nid(const Elf64_Sym& sym) const {
        const auto offset = dynlib + tags.at(0x61000035) + sym.st_name;
        CHECK(offset < file.size());
        std::size_t end = offset;
        while (end < file.size() && file[end] && file[end] != '#') ++end;
        CHECK(end < file.size());
        return {file.data() + offset, end - offset};
    }
    explicit NativeLibc(const char* path) {
        std::ifstream f(path, std::ios::binary);
        CHECK(f.good());
        file = {std::istreambuf_iterator<char>(f), {}};
        const auto eh = get<Elf64_Ehdr>(0);
        CHECK(std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0);
        CHECK(eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_machine == EM_X86_64);
        CHECK(eh.e_phentsize == sizeof(Elf64_Phdr));
        std::vector<Elf64_Phdr> loads;
        Elf64_Phdr dynamic{};
        for (unsigned i = 0; i < eh.e_phnum; ++i) {
            const auto ph = get<Elf64_Phdr>(eh.e_phoff + i * sizeof(Elf64_Phdr));
            if (ph.p_type == PT_LOAD) {
                CHECK(ph.p_vaddr < 16 * 1024 * 1024 && ph.p_memsz < 16 * 1024 * 1024);
                CHECK(ph.p_filesz <= ph.p_memsz && ph.p_offset <= file.size());
                CHECK(ph.p_filesz <= file.size() - ph.p_offset);
                size = std::max(size, static_cast<std::size_t>(ph.p_vaddr + ph.p_memsz));
                loads.push_back(ph);
            } else if (ph.p_type == PT_DYNAMIC) dynamic = ph;
            else if (ph.p_type == 0x61000000) dynlib = ph.p_offset;
        }
        CHECK(size && dynlib && dynamic.p_filesz);
        base = static_cast<unsigned char*>(mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        CHECK(base != MAP_FAILED);
        for (const auto& ph : loads) std::memcpy(base + ph.p_vaddr, file.data() + ph.p_offset, ph.p_filesz);
        for (std::size_t off = 0; off < dynamic.p_filesz; off += sizeof(Elf64_Dyn)) {
            const auto d = get<Elf64_Dyn>(dynamic.p_offset + off);
            tags[d.d_tag] = d.d_un.d_val;
        }
        for (std::size_t off = 0; off < tags.at(0x6100003f); off += sizeof(Elf64_Sym)) {
            const auto sym = get<Elf64_Sym>(dynlib + tags.at(0x61000039) + off);
            if (sym.st_value) exports[nid(sym)] = sym.st_value;
        }
        bool error_bound = false, guard_bound = false;
        for (auto table : {std::pair{0x6100002fULL, 0x61000031ULL},
                           std::pair{0x61000029ULL, 0x6100002dULL}}) {
            for (std::size_t off = 0; off < tags.at(table.second); off += sizeof(Elf64_Rela)) {
                const auto r = get<Elf64_Rela>(dynlib + tags.at(table.first) + off);
                CHECK(r.r_offset <= size && sizeof(std::uint64_t) <= size - r.r_offset);
                std::uint64_t value = 0;
                if (ELF64_R_TYPE(r.r_info) == R_X86_64_RELATIVE) {
                    CHECK(r.r_addend >= 0 && static_cast<std::uint64_t>(r.r_addend) < size);
                    value = reinterpret_cast<std::uint64_t>(base + r.r_addend);
                } else if (ELF64_R_TYPE(r.r_info) == R_X86_64_GLOB_DAT || ELF64_R_TYPE(r.r_info) == R_X86_64_JUMP_SLOT) {
                    const auto sym = get<Elf64_Sym>(dynlib + tags.at(0x61000039) + ELF64_R_SYM(r.r_info) * sizeof(Elf64_Sym));
                    const auto name = nid(sym);
                    if (name == "9BcDykPmo1I") { value = reinterpret_cast<std::uint64_t>(&oracle_error); error_bound = true; }
                    else if (name == "vNe1w4diLCs") value = reinterpret_cast<std::uint64_t>(&oracle_tls);
                    else if (name == "xReSebwKApA") value = reinterpret_cast<std::uint64_t>(&oracle_seek);
                    else if (name == "f7uOxY9mM1U") { value = reinterpret_cast<std::uint64_t>(&oracle_guard); guard_bound = true; }
                    else if (sym.st_value) value = reinterpret_cast<std::uint64_t>(base + sym.st_value);
                    else value = reinterpret_cast<std::uint64_t>(&unresolved_import);
                } else continue;
                std::memcpy(base + r.r_offset, &value, sizeof(value));
            }
        }
        CHECK(error_bound && guard_bound);
    }
    template <typename Fn> Fn function(const char* name) const {
        const auto at = exports.at(name);
        CHECK(at < size);
        return reinterpret_cast<Fn>(base + at);
    }
    template <typename Fn> Fn called_function(const char* name, std::size_t offset) const {
        const auto at = exports.at(name);
        CHECK(at < size && offset <= size - at && 5 <= size - at - offset);
        CHECK(base[at + offset] == 0xe8);
        std::int32_t displacement;
        std::memcpy(&displacement, base + at + offset + 1, sizeof(displacement));
        const auto target = static_cast<std::int64_t>(at + offset + 5) + displacement;
        CHECK(target >= 0 && static_cast<std::uint64_t>(target) < size);
        return reinterpret_cast<Fn>(base + target);
    }
    ~NativeLibc() { if (base) munmap(base, size); }
};
}  // namespace

int main() {
    const char* path = std::getenv("BBHOST_LIBC_ELF");
    if (!path || !*path) { std::puts("SKIP: set BBHOST_LIBC_ELF to the decrypted guest libc.elf"); return 77; }
    using namespace bb::libc;
    NativeLibc lib(path);
    const auto div = lib.function<Div64(*)(std::int64_t, std::int64_t)>("gfP0im5Z3g0");
    const auto test = lib.function<std::int16_t(*)(const double*)>("b-xTWRgI1qw");
    const auto log = lib.function<double(*)(double, int)>("zXCi78bYrEI");
    const auto logf = lib.function<float(*)(float, int)>("jjjRS7l1MPM");
    const auto signed_parse = lib.function<std::int64_t(*)(const char*, char**, int)>("7pNKcscKrf8");
    const auto unsigned_parse = lib.function<std::uint64_t(*)(const char*, char**, int)>("q+9E0X3aWpU");
    const auto wide_parse = lib.function<std::int64_t(*)(const char16_t*, char16_t**, int)>("34nH7v2xvNQ");
    // Execute the native position converter with an empty narrow FILE.
    // Its descriptor seek is supplied above; FILE layout and output writes
    // are performed by the actual guest instructions.
    alignas(8) unsigned char stream[136]{};
    stream[0] = 1;
    const int fd = 7;
    std::memcpy(stream + 4, &fd, sizeof(fd));
    Fpos position{};
    const auto native_position = lib.function<std::int64_t(*)(void*, Fpos*)>("dYJJbxnyb74");
    CHECK(native_position(stream, &position) == 0);
    CHECK(position.offset == 0x100000123LL && position.state[0] == 0 && position.state[1] == 0);
    std::memset(&position, 0xa5, sizeof(position));
    const Fpos unchanged = position;
    oracle_seek_result = static_cast<std::int32_t>(0x80020009u);  // console EBADF
    CHECK(native_position(stream, &position) == -1 && oracle_errno == 9);
    CHECK(std::memcmp(&position, &unchanged, sizeof(position)) == 0);
    // Follow the audited direct calls to read/write preparation helpers.
    // They don't require the native FILE mutex lifecycle on rejected modes.
    const auto write_prep = lib.called_function<int(*)(void*)>("aZK8lNei-Qw", 0x25);
    const auto read_prep = lib.called_function<int(*)(void*)>("AEuF3F2f8TA", 0x61);
    for (const auto [mode, prep] : {std::pair{1u, write_prep}, std::pair{2u, read_prep}}) {
        alignas(8) unsigned char wrong_direction[136]{};
        wrong_direction[0] = static_cast<unsigned char>(mode);
        oracle_errno = 73;
        CHECK(prep(wrong_direction) == -1 && oracle_errno == 73);
        std::uint16_t flags;
        std::memcpy(&flags, wrong_direction, sizeof(flags));
        CHECK(flags & 0x200);
    }
    const auto native_swprintf = lib.function<int(*)(char16_t*, std::size_t, const char16_t*, ...)>("nJz16JE1txM");
    const auto native_swprintf_s = lib.function<int(*)(char16_t*, std::size_t, const char16_t*, ...)>("Im55VJ-Bekc");
    bool compare_secure = true;
    auto compare_format = [&](std::size_t n, const char16_t* fmt, auto... args) {
        char16_t native[300], host[300];
        std::fill(std::begin(native), std::end(native), 0xa5a5);
        std::fill(std::begin(host), std::end(host), 0xa5a5);
        const auto a = native_swprintf(native, n, fmt, args...);
        const auto b = oracle_wide_format(host, n, fmt, args...);
        CHECK(a == b && std::memcmp(native, host, sizeof(host)) == 0);
        if (a >= 0 && n && compare_secure) {
            char16_t secure[300];
            std::fill(std::begin(secure), std::end(secure), 0xa5a5);
            CHECK(native_swprintf_s(secure, n, fmt, args...) == a);
            CHECK(std::memcmp(secure, host, sizeof(host)) == 0);
        }
    };
    compare_format(300, u"%ls%ls%ls", u"title:/Shader/", u"Tex1", u".vpo");
    compare_format(128, u"syntax error at line %d near: ", 137);
    compare_format(300, u"\u3042[%*.*ls][%-4lc][%s][%c]", 5, 2, u"\u00e9\u3042\u03a9", 0x03a9, "ASCII", 'x');
    compare_format(300, u"%ld|%lld|%.2f|%d", 1234567890123LL, -9876543210LL, 1.25, -7);
    compare_format(300, u"%d %d %d %d %d %d %d %d %d %.1f", 1,2,3,4,5,6,7,8,9,2.5);
    for (std::size_t n = 0; n < 7; ++n) compare_format(n, u"abcd");
    compare_format(300, u"%ls", std::u16string(270, u'\u3042').c_str());
    compare_format(300, u"[%s][%3.1s]", "\xc3\xa9\xe3\x81\x82", "\xc3\xa9\xe3\x81\x82");
    compare_format(0, u"");
    // Secure formatter rejects null %s through its runtime-constraint
    // handler; that policy/import is deliberately outside this oracle.
    compare_secure = false;
    compare_format(300, u"[%s][%ls]", static_cast<const char*>(nullptr), static_cast<const char16_t*>(nullptr));
    oracle_errno = 73;
    char16_t invalid[20];
    CHECK(native_swprintf(invalid, 20, u"prefix%c", 0xe9) == -1 && oracle_errno == 73);
    oracle_errno = 73;
    CHECK(native_swprintf(invalid, 20, u"prefix%s", "\xff") == -1 && oracle_errno == 86);
    std::mt19937_64 rng(37);
    const char* extra[] = {"0x", "0xg", "0x1", "0778", "--12", "+-12", "-+12", "++12", " - 12",
        "9223372036854775808", "-9223372036854775809", "18446744073709551616", "-18446744073709551616"};
    auto compare_signed = [&](const char* s, int radix) {
        char* end;
        oracle_errno = 0;
        const auto n = signed_parse(s, &end, radix);
        const auto h = parse_signed(s, radix);
        CHECK(n == h.value && end == h.end && oracle_errno == (h.range_error ? 34 : 0));
    };
    auto compare_wide = [&](const char16_t* s, int radix) {
        char16_t* end;
        oracle_errno = 0;
        const auto n = wide_parse(s, &end, radix);
        const auto h = parse_signed(s, radix);
        CHECK(n == h.value && end == h.end && oracle_errno == (h.range_error ? 34 : 0));
    };
    for (int i = 0; i < 20000; ++i) {
        const auto n = static_cast<std::int64_t>(rng());
        const auto d = static_cast<std::int64_t>(rng() >> 32) + 1;
        const auto native = div(n, d), host = divide(n, d);
        CHECK(native.quot == host.quot && native.rem == host.rem);
        const std::uint64_t bits = rng();
        double x;
        std::memcpy(&x, &bits, sizeof(x));
        CHECK(test(&x) == dtest(&x));
        const double positive = (rng() % 1000000 + 1) * 0.1;
        for (int selector = -1; selector <= 1; ++selector) {
            const auto a = log(positive, selector), b = logarithm(positive, selector);
            CHECK(std::abs(a - b) <= 2e-13 * std::max(1.0, std::abs(a)));
            const auto af = logf(static_cast<float>(positive), selector), bf = logarithm(static_cast<float>(positive), selector);
            CHECK(std::abs(af - bf) <= 3e-6f * std::max(1.0f, std::abs(af)));
        }
        std::string text = i % 3 ? std::to_string(rng()) : std::to_string(n);
        if (i % 7 == 0) text = std::string(rng() % 100, '0') + text;
        const int radix = i % 41 - 2;
        compare_signed(text.c_str(), radix);
        char* end;
        oracle_errno = 0;
        const auto a = unsigned_parse(text.c_str(), &end, radix);
        const auto b = parse_unsigned(text.c_str(), radix);
        CHECK(a == b.value && end == b.end && oracle_errno == (b.range_error ? 34 : 0));
        const std::u16string wide(text.begin(), text.end());
        compare_wide(wide.c_str(), radix);
    }
    for (const char* s : extra) for (int radix = 0; radix <= 36; ++radix) compare_signed(s, radix);
    for (unsigned c = 1; c < 65536; ++c) {
        const char16_t wide[] = {static_cast<char16_t>(c), u'1', u'2', 0};
        compare_wide(wide, 10);
    }
    const auto native_stod = lib.function<double(*)(const char*, char**, std::int64_t)>("c41UEHVtiEA");
    const auto native_wstod = lib.function<double(*)(const char16_t*, char16_t**, std::int64_t)>("4nRn+exUJAM");
    const auto native_wstof = lib.function<float(*)(const char16_t*, char16_t**, std::int64_t)>("GUuiOcxL-r0");
    for (const char* text : {"1e999", "-1e999", "1e-999", "-1e-999", "0e999", "-0e-999",
                            "inf", "nan", "1e-40", "1e-308", "1e-324", "0x1p-2000", "0x1p1024", "nope"}) {
        const std::u16string wide(text, text + std::strlen(text));
        char* narrow_end;
        char16_t* wide_end;
        oracle_errno = 73;
        const auto d = native_stod(text, &narrow_end, 0);
        const auto hd = parse_float<double>(text);
        CHECK(oracle_errno == (hd.range_error ? 34 : 73) && narrow_end == hd.end);
        oracle_errno = 73;
        const auto wd = native_wstod(wide.c_str(), &wide_end, 0);
        const auto hwd = parse_wide_float<double>(wide.c_str());
        CHECK(oracle_errno == (hwd.range_error ? 34 : 73) && wide_end == hwd.end);
        oracle_errno = 73;
        const auto wf = native_wstof(wide.c_str(), &wide_end, 0);
        const auto hwf = parse_wide_float<float>(wide.c_str());
        CHECK(oracle_errno == (hwf.range_error ? 34 : 73) && wide_end == hwf.end);
        // Range/endpoint contract only: native subnormal rounding is NOT
        // certified by these checks. Preserve the already documented gap.
        CHECK(std::isinf(d) == std::isinf(hd.value) && (d == 0) == (hd.value == 0));
        CHECK(std::isinf(wd) == std::isinf(hwd.value) && (wd == 0) == (hwd.value == 0));
        CHECK(std::isinf(wf) == std::isinf(hwf.value) && (wf == 0) == (hwf.value == 0));
    }
    for (const auto& text : {std::u16string(100, u'0') + u"2.5!", std::u16string(u"1.0000000596046448")}) {
        char16_t *native_end, *host_end;
        const double native = native_wstod(text.c_str(), &native_end, 0);
        const double host = wide_stod(text.c_str(), &host_end);
        CHECK(native == host && native_end == host_end);
        CHECK(native_wstof(text.c_str(), &native_end, 0) == static_cast<float>(host));
        CHECK(native_end == host_end);
    }
    for (unsigned c = 128; c < 65536; ++c) {
        const char16_t text[] = {u'2', static_cast<char16_t>(c), u'3', 0};
        char16_t *native_end, *host_end;
        CHECK(native_wstod(text, &native_end, 0) == wide_stod(text, &host_end));
        CHECK(native_end == host_end && native_end == text + 1);
    }
    const auto native_strerror = lib.function<const char*(*)(int)>("RIa6GnWp+iU");
    for (int e = 0; e < 95; ++e) CHECK(std::strcmp(native_strerror(e), error_string(e)) == 0);
    for (int e : {-2147483647-1, -1, 95, 151, 152, 153, 1040, 1050, 1051, 1056, 1062, 1064, 1074, 2147483647})
        CHECK(std::strcmp(native_strerror(e), error_string(e)) == 0);
    const auto lc = lib.function<const Lconv*(*)()>("0hlfW1O4Aa4")();
    const Lconv expected;
    CHECK(std::strcmp(lc->decimal_point, expected.decimal_point) == 0);
    CHECK(std::memcmp(lc->monetary_flags, expected.monetary_flags, 14) == 0);
    std::puts("native libc: 20000 vectors, all log selectors, parsers and 65535 UTF-16 characters matched");
}
