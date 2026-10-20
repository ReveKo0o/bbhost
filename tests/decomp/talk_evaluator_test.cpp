// The talk scripts' evaluator (decomp/talk/evaluator.cpp) against the game's
// own, sub_2b73910, in the loaded eboot (tests/decomp/eboot_kit.h): the same
// expression run by both on two states built alike - a machine (variables,
// call arguments, a child's return value, an environment), a context (a
// stack and registers holding ints, floats and the game's own strings), an
// allocator - and the states compared after: the result, every stack slot
// and register byte for byte (strings by their text), the stack pointer, the
// environment's call log, every string's count and which strings were freed.
//
// The expressions: every condition and argument of the dump's 271 talk
// scripts and its character scripts (BBHOST_APP0; skipped without it), each
// on several states, and random well-formed bytecode over every opcode the
// scripts can use, with the values where the bits matter (NaN payloads, -0,
// denormals, the int range's edges, division by zero, strings). The
// environment answers a hash of the call, sometimes with a new string.
//
// Stubbed in the image: the heap lookup a string's destruction asks
// (sub_247b720: our allocator) and the wide-string imports. Skips without the
// 1.09 eboot.
#include "decomp/talk/evaluator.cpp"

#include "eboot_kit.h"
#include "engine/esd.h"
#include "gcn/container.h"
#include "test_app0.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/mman.h>

// What decomp.cpp provides in the game.

namespace {

using talk_script::Call;
using talk_script::Context;
using talk_script::String;
using talk_script::Value;
using talk_script::kFloat;
using talk_script::kInt;
using talk_script::kString;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

const char* const kTest = "decomp_talk_evaluator";

// ---- The allocator the strings come from, and its record ----

struct Heap {
    std::vector<void*> live;  // in allocation order
    std::vector<void*> freed;
};
Heap* g_heap = nullptr;  // the run's

struct Allocator {
    void** vtable;
} g_alloc;
void* g_alloc_vtable[32];

GUEST_ABI void* alloc_flags(void* out, Allocator*, int) {
    const u64 flags = 0x20;  // a heap that hands out memory
    std::memcpy(out, &flags, 8);
    return out;
}
GUEST_ABI void* alloc_get(Allocator*, u64 size, u64 align) {
    void* p = nullptr;
    if (posix_memalign(&p, align < 16 ? 16 : align, size ? size : 1) != 0) return nullptr;
    std::memset(p, 0xcd, size);
    g_heap->live.push_back(p);
    return p;
}
GUEST_ABI void alloc_free(Allocator*, void* p) {
    if (!p) return;
    g_heap->freed.push_back(p);
    // Kept (not freed) so a later read of it in either run reads the same.
}
template <int N>
GUEST_ABI void alloc_trap() {
    std::printf("decomp_talk_evaluator: the allocator's slot %d (+0x%x) was called\n", N, N * 8);
    std::abort();
}
template <int... I>
void fill_traps(std::integer_sequence<int, I...>) {
    ((g_alloc_vtable[I] = reinterpret_cast<void*>(&alloc_trap<I>)), ...);
}

GUEST_ABI Allocator* heap_of(void*) { return &g_alloc; }  // sub_247b720
GUEST_ABI u16* wmemcpy16(u16* d, const u16* s, u64 n) {
    std::memcpy(d, s, 2 * n);
    return d;
}
GUEST_ABI u16* wmemset16(u16* d, u16 c, u64 n) {
    for (u64 i = 0; i < n; ++i) d[i] = c;
    return d;
}
GUEST_ABI u16* wmemmove16(u16* d, const u16* s, u64 n) {
    std::memmove(d, s, 2 * n);
    return d;
}
GUEST_ABI u64 wcslen16(const u16* s) {
    u64 n = 0;
    while (s[n]) ++n;
    return n;
}

// The C library's tables the eboot's own printf engine asks for, as bbhost
// answers them (hle/libc.cpp): Dinkumware's ctype bits.
short g_ctype[257], g_tolower[257], g_toupper[257];
struct { const char* decimal_point = "."; const char* thousands_sep = ""; const char* grouping = ""; char pad[256] = {}; } g_lconv;
void init_ctype() {
    for (int i = 0; i < 257; ++i) {
        const int c = i - 1;
        short bits = 0;
        if (c >= 0 && c < 128) {
            if (c >= '0' && c <= '9') bits |= 0x020 | 0x001;
            if (c >= 'A' && c <= 'Z') bits |= 0x002;
            if (c >= 'a' && c <= 'z') bits |= 0x010;
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) bits |= 0x001;
            if (c == ' ' || c == '\t') bits |= 0x080;
            if (c == ' ' || (c >= '\t' && c <= '\r')) bits |= 0x004;
            if (c < 0x20 || c == 0x7f) bits |= 0x040;
            if (c > 0x20 && c < 0x7f && !(bits & (0x020 | 0x002 | 0x010))) bits |= 0x008;
        }
        g_ctype[i] = bits;
        g_tolower[i] = static_cast<short>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        g_toupper[i] = static_cast<short>(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
}
GUEST_ABI short* getpctype() { return g_ctype + 1; }
GUEST_ABI short* getptolower() { return g_tolower + 1; }
GUEST_ABI short* getptoupper() { return g_toupper + 1; }
GUEST_ABI void* get_lconv() { return &g_lconv; }

void patch_jump(u64 at, const void* to) {
    u8* p = eboot_kit::at(at);
    const u64 page = at & ~0xfffull;
    mprotect(reinterpret_cast<void*>(page), 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    p[0] = 0xff;
    p[1] = 0x25;
    std::memset(p + 2, 0, 4);
    const u64 a = reinterpret_cast<u64>(to);
    std::memcpy(p + 6, &a, 8);
}

// ---- The game's own strings ----

using GameAlloc = GUEST_ABI void* (*)(u64, u64, void*);
using StringCtor = GUEST_ABI void (*)(String*, void*);
using StringText = GUEST_ABI u8* (*)(String*);
using Assign = GUEST_ABI void (*)(u8*, const u16*, u64);

String* make_string(const std::u16string& text, int count) {
    auto* s = static_cast<String*>(eboot_kit::fn<GameAlloc>(0x247c4e0)(0x48, 8, &g_alloc));
    eboot_kit::fn<StringCtor>(0x2b3df10)(s, &g_alloc);
    eboot_kit::fn<Assign>(0x2a2a720)(eboot_kit::fn<StringText>(0x2b3dfc0)(s),
                                    reinterpret_cast<const u16*>(text.data()), text.size());
    s->count = count;
    return s;
}

std::u16string text(const String* s) { return talk_script::text_of(const_cast<String*>(s)); }

// ---- A state: everything one evaluation reads and writes ----

std::u16string random_text(eboot_kit::Rng& r) {
    // No '%': a text the formatter takes as its format would print other
    // values - string objects among them, whose addresses differ per run.
    static const char16_t kChars[] = u"abcXYZ019 d.-\u3042\u30ad\u2014";
    std::u16string t;
    const int n = r.chance(0.3) ? r.range(8, 24) : r.range(0, 7);  // in place, and on the heap
    for (int i = 0; i < n; ++i) t += kChars[r.range(0, static_cast<int>(std::size(kChars)) - 2)];
    return t;
}

float special_float(eboot_kit::Rng& r) {
    static const u32 kBits[] = {0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0x00000001, 0x80000001,
                                0x007fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fc12345, 0x7f812345,
                                0x4f000000, 0xcf000000, 0x4effffff, 0x501502f9, 0x40490fdb, 0x447a0000, 0x3dcccccd};
    float f;
    if (r.chance(0.5)) {
        std::memcpy(&f, &kBits[r.range(0, static_cast<int>(std::size(kBits)) - 1)], 4);
        return f;
    }
    if (r.chance(0.5)) return static_cast<float>(r.range(-100, 100));
    return r.uniform(-1000.0f, 1000.0f);
}

u64 garbage(eboot_kit::Rng& r) { return r.next() & 0xffffffff00000000ull; }

struct Env {
    void** vtable;
    int machine_tag;
};
void* g_env_vtable[3];

struct State {
    Heap heap;
    Value under[4];  // what a pop below the stack would read
    Context ctx;
    u8 machine[0x200];
    Value vars[32], args[16];
    u8 child[0x100];
    Env env;
    std::vector<String*> strings;  // made for the state, in order
    std::vector<std::string> calls;
};
State* g_state = nullptr;  // the run's

Value make_value(eboot_kit::Rng& r, State& s, double string_chance) {
    Value v;
    v.pad = static_cast<u32>(r.next());
    const double p = r.uniform(0, 1);
    if (p < string_chance) {
        String* str = make_string(random_text(r), 1);
        s.strings.push_back(str);
        v.type = kString;
        v.payload = reinterpret_cast<u64>(str);
    } else if (p < 0.6) {
        v.type = kInt;
        const u32 i = r.chance(0.3) ? static_cast<u32>(r.range(-4, 40)) : r.u32();
        v.payload = garbage(r) | i;
    } else if (p < 0.97) {
        v.type = kFloat;
        const float f = special_float(r);
        u32 b;
        std::memcpy(&b, &f, 4);
        v.payload = garbage(r) | b;
    } else {
        v.type = static_cast<u32>(r.range(4, 9));  // a type nothing writes: counts as 0.0
        v.payload = r.next();
    }
    return v;
}

// A state from a seed; built twice, it is built alike (strings aside, which
// are new objects with the same texts and counts).
void build(State& s, u64 seed, int start) {
    eboot_kit::Rng r(seed);
    g_heap = &s.heap;
    for (Value& v : s.under) v = Value{0x1111111100000007ull, kInt, 0};
    for (int i = 0; i < 64; ++i) s.ctx.stack[i] = make_value(r, s, 0.12);
    for (Value& v : s.ctx.reg) v = make_value(r, s, 0.15);
    s.ctx.sp = s.ctx.stack + start;
    s.ctx.allocator = &g_alloc;
    std::memset(s.machine, 0xab, sizeof s.machine);
    for (Value& v : s.vars) v = make_value(r, s, 0.15);
    for (Value& v : s.args) v = make_value(r, s, 0.15);
    std::memset(s.child, 0xab, sizeof s.child);
    Value ret = r.chance(0.2) ? Value{0x7fffffff, kInt, 0} : make_value(r, s, 0.2);
    std::memcpy(s.child + talk_script::kChildReturn, &ret, sizeof ret);
    s.env = Env{g_env_vtable, 7};
    auto put = [&](std::size_t off, const void* p) { std::memcpy(s.machine + off, &p, 8); };
    put(talk_script::kMachineVariables, s.vars);
    put(talk_script::kMachineArguments, s.args);
    put(talk_script::kMachineChild, r.chance(0.8) ? static_cast<const void*>(s.child) : nullptr);
    put(talk_script::kMachineEnv, r.chance(0.95) ? static_cast<const void*>(&s.env) : nullptr);
}

// The environment: logs each call, answers a hash of it.
u64 mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

GUEST_ABI Value* env_query(Value* out, Env* env, Call* c, u8* machine) {
    std::string line = "id " + std::to_string(static_cast<std::int32_t>(c->v[0].payload)) + " n " + std::to_string(c->count) +
                       " m " + std::to_string(env->machine_tag) + (machine == g_state->machine ? "" : " other machine");
    u64 h = static_cast<u32>(c->v[0].payload);
    for (int i = 1; i < c->count && i < 8; ++i) {
        const Value& a = c->v[i];
        line += " | " + std::to_string(a.type) + ":";
        h = mix(h, a.type);
        if (a.type == kString) {
            const std::u16string t = text(reinterpret_cast<const String*>(a.payload));
            for (char16_t ch : t) {
                line += std::to_string(static_cast<int>(ch)) + ",";
                h = mix(h, ch);
            }
        } else {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(a.payload));
            line += buf;
            h = mix(h, a.payload);
        }
    }
    g_state->calls.push_back(line);
    eboot_kit::Rng r(h);
    *out = make_value(r, *g_state, 0.1);
    if (out->type == kString) g_state->strings.pop_back();  // the caller's now
    return out;
}

// ---- Running and comparing ----

using Evaluate = GUEST_ABI Value* (*)(Value*, u8*, Context*, const u8*);

Value run(State& s, bool game, const u8* code) {
    g_state = &s;
    g_heap = &s.heap;
    Value out{0x5555555555555555ull, 0x55555555, 0x55555555};
    if (game) eboot_kit::fn<Evaluate>(0x2b73910)(&out, s.machine, &s.ctx, code);
    else talk_script::evaluate(&out, s.machine, &s.ctx, code);
    return out;
}

std::string describe(const Value& v) {
    char buf[96];
    if (v.type == kString) {
        std::string t;
        for (char16_t c : text(reinterpret_cast<const String*>(v.payload))) t += c < 128 ? static_cast<char>(c) : '?';
        std::snprintf(buf, sizeof buf, "string \"%s\"", t.c_str());
    } else {
        std::snprintf(buf, sizeof buf, "type %u payload %016llx pad %08x", v.type, static_cast<unsigned long long>(v.payload), v.pad);
    }
    return buf;
}

// Equal values: a string by its text, an int or a float by its 32 bits, any
// other type byte for byte; the pad, which nothing writes, as is. The high
// half of an int's or a float's payload is not compared: the values are
// written 4 bytes at a time, so it is whatever the slot held - which both
// runs agree on - except after a call with no environment, where the game's
// answer is a local of its handler's frame with only its low half written.
bool same_value(const Value& a, const Value& b) {
    if (a.type != b.type || a.pad != b.pad) return false;
    if (a.type == kString) return text(reinterpret_cast<const String*>(a.payload)) == text(reinterpret_cast<const String*>(b.payload));
    if (a.type == kInt || a.type == kFloat) return static_cast<u32>(a.payload) == static_cast<u32>(b.payload);
    return a.payload == b.payload;
}

// Where a value's string is among the state's own (or -1: a new one).
int string_index(const State& s, u64 p) {
    for (std::size_t i = 0; i < s.strings.size(); ++i)
        if (reinterpret_cast<u64>(s.strings[i]) == p) return static_cast<int>(i);
    return -1;
}

int g_reported = 0;

bool differs(const char* what, int i, const std::string& theirs, const std::string& ours, const std::vector<u8>& code) {
    if (++g_reported <= 12) {
        std::printf("%s %d: the game's %s, ours %s; code", what, i, theirs.c_str(), ours.c_str());
        for (std::size_t k = 0; k < code.size() && k < 48; ++k) std::printf(" %02x", code[k]);
        std::printf("\n");
    }
    return false;
}

bool compare(const State& a, const Value& ra, const State& b, const Value& rb, const std::vector<u8>& code) {
    if (!same_value(ra, rb))
        return differs("result", static_cast<int>(a.calls.size() * 100 + b.calls.size()), describe(ra), describe(rb), code);
    if (ra.type == kString &&
        string_index(a, ra.payload) != string_index(b, rb.payload))
        return differs("result string", 0, std::to_string(string_index(a, ra.payload)), std::to_string(string_index(b, rb.payload)),
                       code);
    if (a.ctx.sp - a.ctx.stack != b.ctx.sp - b.ctx.stack)
        return differs("stack pointer", 0, std::to_string(a.ctx.sp - a.ctx.stack), std::to_string(b.ctx.sp - b.ctx.stack), code);
    for (int i = 0; i < 64; ++i)
        if (!same_value(a.ctx.stack[i], b.ctx.stack[i]))
            return differs("stack slot", i, describe(a.ctx.stack[i]), describe(b.ctx.stack[i]), code);
    for (int i = 0; i < 8; ++i)
        if (!same_value(a.ctx.reg[i], b.ctx.reg[i])) return differs("register", i, describe(a.ctx.reg[i]), describe(b.ctx.reg[i]), code);
    for (int i = 0; i < 4; ++i)
        if (!same_value(a.under[i], b.under[i])) return differs("below the stack", i, describe(a.under[i]), describe(b.under[i]), code);
    if (a.calls != b.calls) {
        const std::size_t n = std::min(a.calls.size(), b.calls.size());
        std::size_t k = 0;
        while (k < n && a.calls[k] == b.calls[k]) ++k;
        return differs("call", static_cast<int>(k), k < a.calls.size() ? a.calls[k] : "none", k < b.calls.size() ? b.calls[k] : "none",
                       code);
    }
    for (std::size_t i = 0; i < a.strings.size() && i < b.strings.size(); ++i)
        if (a.strings[i]->count != b.strings[i]->count)
            return differs("string count", static_cast<int>(i), std::to_string(a.strings[i]->count), std::to_string(b.strings[i]->count),
                           code);
    // What was freed: the same strings of the state's, and as many others.
    auto freed_of = [](const State& s) {
        std::vector<int> mine;
        int others = 0;
        for (void* p : s.heap.freed) {
            const int i = string_index(s, reinterpret_cast<u64>(p));
            if (i >= 0) mine.push_back(i);
            else ++others;
        }
        return std::make_pair(mine, others);
    };
    if (freed_of(a) != freed_of(b))
        return differs("freed", 0, std::to_string(a.heap.freed.size()), std::to_string(b.heap.freed.size()), code);
    if (a.heap.live.size() != b.heap.live.size())
        return differs("allocations", 0, std::to_string(a.heap.live.size()), std::to_string(b.heap.live.size()), code);
    return true;
}

int g_cases = 0, g_bad = 0;

void check(const std::vector<u8>& code, u64 seed) {
    for (int start : {0, 3}) {
        auto* a = new State;
        auto* b = new State;
        build(*a, seed, start);
        build(*b, seed, start);
        const Value ra = run(*a, true, code.data());
        const Value rb = run(*b, false, code.data());
        ++g_cases;
        if (!compare(*a, ra, *b, rb, code)) ++g_bad;
        for (void* q : a->heap.live) std::free(q);
        for (void* q : b->heap.live) std::free(q);
        delete a;
        delete b;
    }
}

// ---- Random well-formed bytecode ----

struct Gen {
    eboot_kit::Rng& r;
    std::vector<u8> out;
    int depth = 0;  // values this expression pushed and has not popped
    int max_depth = 0;

    void emit(u8 b) { out.push_back(b); }
    void emit4(u32 v) {
        for (int i = 0; i < 4; ++i) emit(static_cast<u8>(v >> (8 * i)));
    }
    void pushed(int n) {
        depth += n;
        if (depth > max_depth) max_depth = depth;
    }

    void literal() {
        const int k = r.range(0, 9);
        if (k < 3) {
            emit(static_cast<u8>(r.range(0, 0x7f)));
        } else if (k < 5) {
            emit(0x80);
            const float f = special_float(r);
            u32 b;
            std::memcpy(&b, &f, 4);
            emit4(b);
        } else if (k < 7) {
            emit(0x81);
            double d;
            static const u64 kBits[] = {0x0000000000000000ull, 0x8000000000000000ull, 0x41dfffffffc00000ull, 0xc1e0000000000000ull,
                                        0x41e0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000001ull, 0x7ff0000000000000ull,
                                        0x0000000000000001ull, 0x3fe0000000000000ull, 0x4415af1d78b58c40ull, 0x3ff0000000000001ull};
            if (r.chance(0.5)) std::memcpy(&d, &kBits[r.range(0, static_cast<int>(std::size(kBits)) - 1)], 8);
            else d = r.chance(0.5) ? static_cast<double>(r.range(-100000, 100000)) : static_cast<double>(r.uniform(-1e6f, 1e6f)) / 7.0;
            u64 b;
            std::memcpy(&b, &d, 8);
            for (int i = 0; i < 8; ++i) emit(static_cast<u8>(b >> (8 * i)));
        } else if (k < 8) {
            emit(0x82);
            static const u32 kInts[] = {0x7fffffff, 0x80000000, 0xffffffff, 0, 1, 0x40000000, 0x7ffffffe};
            emit4(r.chance(0.6) ? kInts[r.range(0, static_cast<int>(std::size(kInts)) - 1)] : r.u32());
        } else {
            emit(0xa5);
            for (char16_t c : random_text(r)) {
                emit(static_cast<u8>(c));
                emit(static_cast<u8>(c >> 8));
            }
            emit(0);
            emit(0);
        }
        pushed(1);
    }

    // One value pushed, of `budget` nodes at most.
    void value(int budget) {
        const int k = budget <= 1 ? r.range(0, 3) : r.range(0, 13);
        switch (k) {
        case 0: case 1: literal(); return;
        case 2: emit(static_cast<u8>(0xaf + r.range(0, 7))); pushed(1); return;
        case 3: emit(r.chance(0.5) ? 0xb9 : 0xba); pushed(1); return;
        case 4: {  // unary
            static const u8 kOps[] = {0x8d, 0x97, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0};
            value(budget - 1);
            emit(kOps[r.range(0, static_cast<int>(std::size(kOps)) - 1)]);
            return;
        }
        case 5: case 6: {  // binary
            static const u8 kOps[] = {0x8c, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x98, 0x99};
            value(budget / 2);
            value(budget / 2);
            emit(kOps[r.range(0, static_cast<int>(std::size(kOps)) - 1)]);
            --depth;
            return;
        }
        case 7: {  // a table entry
            if (r.chance(0.7)) {
                emit(static_cast<u8>(0x40 + r.range(0, 15)));
            } else {
                emit(0x80);
                const float f = static_cast<float>(r.range(0, 150)) / 10.0f;  // truncates in range
                u32 b;
                std::memcpy(&b, &f, 4);
                emit4(b);
            }
            pushed(1);
            emit(r.chance(0.5) ? 0x83 : 0xb8);
            return;
        }
        case 8: case 9: {  // a call
            const int n = r.range(0, 6);
            if (r.chance(0.8)) {
                emit(static_cast<u8>(0x40 + r.range(0, 62)));
                pushed(1);
            } else {
                value(1);  // an id of any kind
            }
            for (int i = 0; i < n; ++i) value(budget / (n + 1));
            emit(static_cast<u8>(0x84 + n));
            depth -= n;
            if (r.chance(0.5)) emit(0xa6);
            return;
        }
        case 10: {  // a string from two or three values
            const int n = r.chance(0.5) ? 2 : 3;
            if (r.chance(0.5)) {
                // A format of the formatter's kind, an int for each of its
                // fields (no more fields than values: one more would read
                // whatever the vararg registers held).
                static const char16_t* kOne[] = {u"%d", u"x=%d", u"%5d|", u"%x", u"%%", u""};
                static const char16_t* kTwo[] = {u"%d %d", u"%d-%x", u"%x%d", u"%d", u"%%"};
                const std::u16string f = n == 2 ? kOne[r.range(0, static_cast<int>(std::size(kOne)) - 1)]
                                                : kTwo[r.range(0, static_cast<int>(std::size(kTwo)) - 1)];
                emit(0xa5);
                for (char16_t c : f) {
                    emit(static_cast<u8>(c));
                    emit(static_cast<u8>(c >> 8));
                }
                emit(0);
                emit(0);
                pushed(1);
                for (int i = 1; i < n; ++i) {
                    emit(0x82);
                    emit4(r.chance(0.5) ? r.u32() : static_cast<u32>(r.range(-50, 50)));
                    pushed(1);
                }
            } else {
                for (int i = 0; i < n; ++i) value(budget / n);
            }
            emit(n == 2 ? 0xa3 : 0xa4);
            depth -= n - 1;
            return;
        }
        case 11: {  // kept in a register
            value(budget - 1);
            emit(static_cast<u8>(0xa7 + r.range(0, 7)));
            return;
        }
        case 12: {  // stop unless true, then go on
            value(budget - 1);
            emit(0xb7);
            if (r.chance(0.5)) emit(0xa2);
            return;
        }
        default: literal(); return;
        }
    }
};

std::vector<u8> random_expression(eboot_kit::Rng& r) {
    Gen g{r};
    g.value(r.range(1, 24));
    g.emit(0xa1);
    return g.out;
}

// ---- The dump's scripts ----

// The opcodes of an expression, literals stepped over; false if it runs off
// its end.
bool opcodes(const std::vector<u8>& code, std::vector<u8>* out) {
    std::size_t i = 0;
    while (i < code.size()) {
        const u8 op = code[i++];
        out->push_back(op);
        if (op == 0xa1) return true;
        if (op == 0x80 || op == 0x82) i += 4;
        else if (op == 0x81) i += 8;
        else if (op == 0xa5) {
            while (i + 1 < code.size() && (code[i] || code[i + 1])) i += 2;
            i += 2;
        }
    }
    return false;
}

std::vector<std::vector<u8>> corpus(int* scripts) {
    namespace fs = std::filesystem;
    std::vector<std::vector<u8>> out;
    *scripts = 0;
    auto add_script = [&](const std::vector<u8>& data) {
        esd::Script s;
        std::string why;
        if (!esd::parse(data, s, &why)) return false;
        ++*scripts;
        for (const esd::Expr& e : s.exprs) out.push_back(e.code);
        return true;
    };
    const std::string talk = test_app0_file("dvdroot_ps4/script/talk");
    std::error_code ec;
    if (talk.empty() || !fs::is_directory(talk, ec)) return out;
    for (const auto& entry : fs::directory_iterator(talk, ec)) {
        if (entry.path().string().find(".talkesdbnd.dcx") == std::string::npos) continue;
        std::ifstream f(entry.path(), std::ios::binary);
        const std::vector<u8> dcx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string why;
        gcn::Bnd4Archive a;
        if (!gcn::bnd4_read(gcn::dcx_decompress(dcx, &why), a, &why)) continue;
        for (const gcn::Bnd4File& file : a.files) add_script(file.data);
    }
    for (const char* chr : {"dvdroot_ps4/chr/c0000.esd.dcx", "dvdroot_ps4/chr/enemycommon.esd.dcx"}) {
        std::ifstream f(test_app0_file(chr), std::ios::binary);
        const std::vector<u8> dcx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string why;
        if (dcx.empty()) continue;
        if (!add_script(gcn::dcx_decompress(dcx, &why))) std::printf("decomp_talk_evaluator: %s is not a long-format script (%s)\n", chr, why.c_str());
    }
    return out;
}

}  // namespace

int main() {
    eboot_kit::load(kTest);
    // The fixes the image needs outside the game: strings' heap lookup and wcslen.
    fill_traps(std::make_integer_sequence<int, 32>{});
    g_alloc_vtable[4] = reinterpret_cast<void*>(&alloc_flags);   // +0x20
    g_alloc_vtable[11] = reinterpret_cast<void*>(&alloc_get);    // +0x58
    g_alloc_vtable[14] = reinterpret_cast<void*>(&alloc_free);   // +0x70
    g_alloc.vtable = g_alloc_vtable;
    g_env_vtable[2] = reinterpret_cast<void*>(&env_query);
    patch_jump(0x247b720, reinterpret_cast<const void*>(&heap_of));
    eboot_kit::bind("wcslen", reinterpret_cast<const void*>(&wcslen16));
    eboot_kit::bind("wmemcpy", reinterpret_cast<const void*>(&wmemcpy16));
    eboot_kit::bind("wmemset", reinterpret_cast<const void*>(&wmemset16));
    eboot_kit::bind("wmemmove", reinterpret_cast<const void*>(&wmemmove16));
    init_ctype();
    eboot_kit::bind("_Getpctype", reinterpret_cast<const void*>(&getpctype));
    eboot_kit::bind("_Getptolower", reinterpret_cast<const void*>(&getptolower));
    eboot_kit::bind("_Getptoupper", reinterpret_cast<const void*>(&getptoupper));
    eboot_kit::bind("localeconv", reinterpret_cast<const void*>(&get_lconv));

    // The entry the decomp list places ours at.
    if (std::memcmp(eboot_kit::at(0x2b73910), talk_script::kEntry, sizeof talk_script::kEntry) != 0) {
        std::printf("decomp_talk_evaluator: sub_2b73910 is not where the decomp expects it\n");
        return 1;
    }

    int scripts = 0;
    const std::vector<std::vector<u8>> exprs = corpus(&scripts);
    int used[256] = {}, skipped = 0;
    for (std::size_t i = 0; i < exprs.size(); ++i) {
        std::vector<u8> ops;
        const bool whole = opcodes(exprs[i], &ops);
        for (u8 op : ops) ++used[op];
        // A call of seven arguments overruns the game's call object (ours
        // gives it room): no script has one, and the compare would only see
        // the game's version corrupt its own frame.
        if (!whole || std::find(ops.begin(), ops.end(), 0x8b) != ops.end()) {
            ++skipped;
            continue;
        }
        for (u64 k = 0; k < 3; ++k) check(exprs[i], 0x1000 * i + k);
    }
    const int corpus_cases = g_cases, corpus_bad = g_bad;
    std::printf("decomp_talk_evaluator: %zu expressions of %d scripts (%d left out), %d cases, %d differ\n", exprs.size(), scripts, skipped,
                corpus_cases, corpus_bad);
    std::printf("decomp_talk_evaluator: the opcodes the scripts use:");
    for (int op = 0x80; op < 0x100; ++op)
        if (used[op]) std::printf(" %02x", op);
    int ints = 0;
    for (int op = 0; op < 0x80; ++op) ints += used[op] ? 1 : 0;
    std::printf(" and %d of the 128 small ints\n", ints);

    eboot_kit::Rng r(0x7a1c5c21);
    const int kFuzz = 60000;
    for (int i = 0; i < kFuzz; ++i) {
        const std::vector<u8> code = random_expression(r);
        check(code, r.next());
    }
    std::printf("decomp_talk_evaluator: %d random expressions, %d cases, %d differ\n", kFuzz, g_cases - corpus_cases, g_bad - corpus_bad);

    // The same under the host's floating-point mode (denormals kept).
    eboot_kit::host_fp();
    const int before = g_cases, bad_before = g_bad;
    for (int i = 0; i < kFuzz / 6; ++i) {
        const std::vector<u8> code = random_expression(r);
        check(code, r.next());
    }
    eboot_kit::guest_fp();
    std::printf("decomp_talk_evaluator: %d cases under the host's floating-point mode, %d differ\n", g_cases - before, g_bad - bad_before);
    return g_bad ? 1 : 0;
}
