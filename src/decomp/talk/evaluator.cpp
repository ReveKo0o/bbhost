// The talk scripts' expression evaluator, as ours (docs/decomp.md):
// sub_2b73910, the loop that Dantelion2's EzState library - the state
// machines of the NPCs' talk scripts (talkesdbnd's .esd files) and of
// EzMenu's menus - evaluates every condition and every command argument with
// ("EzStateEvaluator" among the binary's class names).
//
// An expression is bytecode (engine/esd.h writes it): one byte per opcode,
// literals inline, ended by 0xa1. It runs on a context the machine's Update
// keeps on its stack (decomp/talk/evaluator.h): 64 values, 8 registers, a stack
// pointer. The game dispatches each opcode through a table of 256 handlers
// (0x56c8880); ours is one switch over the same handlers' semantics:
//
//   0x00-0x7f  push the int op - 0x40
//   0x80 0x81  a float / double literal: an int when it converts back exactly
//              (so -0.0 is int 0), else a float
//   0x82       an int literal
//   0x83 0xb8  top = machine variable / call argument [top] (no bounds check)
//   0x84-0x8b  a call with 0-7 arguments: the environment's slot 2 answers it
//              (no environment: float 0)
//   0x8c-0x90  + neg - * /      0x91-0x96  < > <= >= == !=
//   0x97       abs              0x98 0x99  && ||
//   0x9a-0xa0  == 0, < 0, > 0, <= 0, >= 0, == 0, != 0
//   0xa1       end              0xa2 0xa6  nothing
//   0xa3 0xa4  two / three values formatted into a string (the game's own
//              formatter, sub_2b624c0)
//   0xa5       a string literal (UTF-16, to its NUL)
//   0xa7-0xae  register[k] = top     0xaf-0xb6  push register[k]
//   0xb7       stop with int 0 unless the top is true
//   0xb9       push the child machine's return value (none: 0x7fffffff)
//   0xba       push 0x7fffffff, "no value"
//
// Arithmetic and comparisons convert their operands to double (a string
// counts as 0.0) and compute in double; arithmetic pushes an int when
// cvttsd2si round-trips the result exactly, else the float it rounds to. The
// operand order is the game's, NaN payloads included: b + a, a - b, b * a,
// a / b (the asm below). Truth is "not equal to 0.0", so NaN is true - except
// for 0xb7, which truncates its value to an int, so 0.5 is false there.
//
// Each value slot owns its string. A push releases the string the slot held
// before; a pop leaves it in its slot. Copies go through the game's count
// sequence - the source held by two temporaries (three for registers and a
// child's value) while the old value is released - so a string's count moves
// as the game's does, step for step.
//
// Where the game's version would crash ours does the safe thing:
//   0xbb   the game's table has no handler (a call through null): ours ends
//          the expression there
//   0x8b   a call with seven arguments overruns the game's call object (room
//          for seven values, the eighth landing on the stack guard and a
//          saved register): ours gives the call room for eight
// The 68 handlers at 0xbc-0xff (push an int, then its "%d" as a string) are
// used by no script; ours runs the game's own handler for them.
#include "decomp/talk/evaluator.h"

#include "core/tls_rewrite.h"
#include "decomp/decomp.h"
#include "decomp/guest.h"
#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <emmintrin.h>

using namespace decomp;

namespace talk_script {
namespace {

// The game's code and data this calls on (Binary Ninja addresses).
constexpr std::uint64_t kHandlers = 0x56c8880;      // the opcode table
constexpr std::uint64_t kCallVtable = 0x56c8850;    // the call object's class
constexpr std::uint64_t kDlPanic = 0x24b55b0;
constexpr std::uint64_t kRefMessage = 0x4c78c2c;    // DL_PANIC's message for a count below one
constexpr int kRefLine = 0x3e;
constexpr std::uint64_t kFormat = 0x2b624c0;        // (Value* out, Context*, n): pops n, formats a string
constexpr std::uint64_t kAllocate = 0x247c4e0;      // (size, align, allocator)
constexpr std::uint64_t kDefaultAllocator = 0x5940670;
constexpr std::uint64_t kMakeDefaultAllocator = 0x2fa2b10;
constexpr std::uint64_t kStringCtor = 0x2b3df10;    // (String*, allocator)
constexpr std::uint64_t kStringText = 0x2b3dfc0;    // String* -> its 16-bit string (+0x10)
constexpr std::uint64_t kAssign = 0x2a2a720;        // (text, const u16*, length)
constexpr std::uint64_t kWcslen = 0x2fbf228;        // the import, through its PLT entry

using Handler = GUEST_ABI void (*)(std::uint8_t*, Context*, const std::uint8_t**);
using Panic = GUEST_ABI void (*)(int, int, const char*, ...);
using Query = GUEST_ABI Value* (*)(Value*, void*, Call*, std::uint8_t*);
using Destroy = GUEST_ABI void (*)(String*);
using Format = GUEST_ABI Value* (*)(Value*, Context*, int);
using Allocate = GUEST_ABI void* (*)(std::uint64_t, std::uint64_t, void*);
using MakeAllocator = GUEST_ABI void* (*)();
using StringCtor = GUEST_ABI void (*)(String*, void*);
using StringText = GUEST_ABI std::uint8_t* (*)(String*);
using Assign = GUEST_ABI void (*)(std::uint8_t*, const std::uint16_t*, std::uint64_t);
using Wcslen = GUEST_ABI std::uint64_t (*)(const std::uint16_t*);

constexpr std::uint64_t kTalkEnvVtable = 0x575aab0;  // the talk scripts' environment (sub_243ad60 answers)

std::atomic<std::uint64_t> g_evaluations{0}, g_talk_evaluations{0};
std::atomic<std::uint64_t> g_talk_vtable{0};  // kTalkEnvVtable as the guest sees it, once known

// ---- Values and their strings, as the game handles them ----

String* string_of(std::uint64_t payload) { return reinterpret_cast<String*>(static_cast<std::uintptr_t>(payload)); }

inline void addref(String* s) { __atomic_fetch_add(&s->count, 1, __ATOMIC_SEQ_CST); }

// DLReferenceCountObject's release: the last reference destroys the string,
// a count already at zero is the engine's panic.
inline void release(String* s) {
    const std::int32_t was = __atomic_fetch_add(&s->count, -1, __ATOMIC_SEQ_CST);
    if (was == 1) {
        reinterpret_cast<Destroy>(s->vtable[0])(s);
    } else if (was <= 0) {
        game_function<Panic>(kDlPanic)(0, kRefLine, reinterpret_cast<const char*>(static_cast<std::uintptr_t>(game_address(kRefMessage))));
    }
}

inline void release_old(Value* v) {
    if (v->type == kString) release(string_of(v->payload));
}

inline void write_low32(Value* v, std::uint32_t bits) { std::memcpy(&v->payload, &bits, 4); }

// dst = src as the game copies an EzStateValue: the source held by `temps`
// temporaries while the destination's old value is released, all 8 payload
// bytes copied.
inline void assign(Value* dst, const Value* src, int temps) {
    const std::uint32_t type = src->type;
    const std::uint64_t payload = src->payload;
    if (type == kString)
        for (int i = 0; i < temps; ++i) addref(string_of(payload));
    release_old(dst);
    dst->type = type;
    if (type == kString) {
        addref(string_of(payload));
        dst->payload = payload;
        for (int i = 0; i < temps; ++i) release(string_of(payload));
    } else {
        dst->payload = payload;
    }
}

inline Value* push(Context* ctx) { return ctx->sp++; }
inline Value* pop(Context* ctx) { return --ctx->sp; }

inline float as_float(const Value* v) {
    float f;
    std::memcpy(&f, &v->payload, 4);
    return f;
}

inline std::int32_t as_int(const Value* v) {
    std::int32_t i;
    std::memcpy(&i, &v->payload, 4);
    return i;
}

// The conversions as instructions: a compiler may fold float -> double ->
// float into nothing, where the game's quiets a signaling NaN.
inline double widen(float f) {
    double d;
    asm("cvtss2sd %1, %0" : "=x"(d) : "x"(f));
    return d;
}
inline float narrow(double d) {
    float f;
    asm("cvtsd2ss %1, %0" : "=x"(f) : "x"(d));
    return f;
}

// What arithmetic sees: a float or an int as a double, anything else 0.0.
inline double number(const Value* v) {
    if (v->type == kFloat) return widen(as_float(v));
    if (v->type == kInt) return static_cast<double>(as_int(v));
    return 0.0;
}

// The sign bit cleared, as the game's vandpd does (a negative NaN too).
inline double abs_bits(double x) {
    std::uint64_t b;
    std::memcpy(&b, &x, 8);
    b &= 0x7fffffffffffffffull;
    std::memcpy(&x, &b, 8);
    return x;
}

inline std::int32_t truncate(double d) { return _mm_cvttsd_si32(_mm_set_sd(d)); }
inline std::int32_t truncate(float f) { return _mm_cvtt_ss2si(_mm_set_ss(f)); }

inline std::uint32_t float_bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// A result into `slot`: an int when it converts back exactly, else a float.
inline void put_number(Value* slot, double r) {
    const std::int32_t i = truncate(r);
    release_old(slot);
    if (static_cast<double>(i) == r) {
        slot->type = kInt;
        write_low32(slot, static_cast<std::uint32_t>(i));
    } else {
        slot->type = kFloat;
        write_low32(slot, float_bits(narrow(r)));
    }
}

inline void put_int(Value* slot, std::uint32_t i) {
    release_old(slot);
    slot->type = kInt;
    write_low32(slot, i);
}

// The game's operand order: with two NaNs, the first source's payload wins.
inline double add_sd(double x, double y) {
    asm("addsd %1, %0" : "+x"(x) : "x"(y));
    return x;
}
inline double sub_sd(double x, double y) {
    asm("subsd %1, %0" : "+x"(x) : "x"(y));
    return x;
}
inline double mul_sd(double x, double y) {
    asm("mulsd %1, %0" : "+x"(x) : "x"(y));
    return x;
}
inline double div_sd(double x, double y) {
    asm("divsd %1, %0" : "+x"(x) : "x"(y));
    return x;
}

// Two operands: b the top, a under it; the result takes a's slot.
template <class Op>
inline void binary(Context* ctx, Op op) {
    const Value* b = pop(ctx);
    const double vb = number(b);
    Value* a = pop(ctx);
    const double va = number(a);
    push(ctx);
    op(a, va, vb);
}

// 0x84-0x8b: pop the arguments (the last on top) and the id, ask the
// environment, push its answer.
void call(std::uint8_t* machine, Context* ctx, int args) {
    Call c;
    c.vtable = reinterpret_cast<void**>(static_cast<std::uintptr_t>(game_address(kCallVtable)));
    c.count = args + 1;
    for (Value& v : c.v) {
        v.payload = 0;
        v.type = kInt;
    }
    for (int i = args; i >= 1; --i) assign(&c.v[i], pop(ctx), 2);
    const Value* id = pop(ctx);
    const std::int32_t n = id->type == kFloat ? truncate(as_float(id)) : id->type == kInt ? as_int(id) : 0;
    put_int(&c.v[0], static_cast<std::uint32_t>(n));
    Value r{0, 0, 0};
    void* env = *reinterpret_cast<void**>(machine + kMachineEnv);
    if (env) {
        reinterpret_cast<Query>((*static_cast<void***>(env))[2])(&r, env, &c, machine);
    } else {
        r.type = kFloat;
        write_low32(&r, 0);
    }
    for (int i = args > 6 ? 7 : 6; i >= 0; --i) release_old(&c.v[i]);
    assign(push(ctx), &r, 2);
    release_old(&r);
}

// 0x83, 0xb8: the top, as an index, replaced by that entry of a table.
void table_read(std::uint8_t* machine, Context* ctx, std::size_t table) {
    Value* top = pop(ctx);
    const std::int32_t i = truncate(number(top));
    const Value* entry = *reinterpret_cast<Value**>(machine + table) + static_cast<std::int64_t>(i);
    push(ctx);
    assign(top, entry, 2);
}

// 0xa5: a new string of the literal at *ip, pushed; ip past its NUL.
void string_literal(Context* ctx, const std::uint8_t*& ip) {
    const auto* lit = reinterpret_cast<const std::uint16_t*>(ip);
    void* alloc = ctx->allocator;
    if (!alloc) {
        void** slot = reinterpret_cast<void**>(static_cast<std::uintptr_t>(game_address(kDefaultAllocator)));
        alloc = *slot;
        if (!alloc) *slot = alloc = game_function<MakeAllocator>(kMakeDefaultAllocator)();
    }
    auto* s = static_cast<String*>(game_function<Allocate>(kAllocate)(0x48, 8, alloc));
    if (!s) {
        // The game's goes on to count a null string and faults: ours pushes
        // int 0 and skips the literal.
        std::size_t n = 0;
        while (lit[n]) ++n;
        ip += 2 * n + 2;
        put_int(push(ctx), 0);
        return;
    }
    game_function<StringCtor>(kStringCtor)(s, alloc);
    addref(s);
    const std::uint64_t len = *lit ? game_function<Wcslen>(kWcslen)(lit) : 0;
    game_function<Assign>(kAssign)(game_function<StringText>(kStringText)(s), lit, len);
    const std::uint8_t* text = game_function<StringText>(kStringText)(s);
    std::uint64_t capacity;
    std::memcpy(&capacity, text + 0x20, 8);
    const std::uint16_t* chars;
    if (capacity >= 8) std::memcpy(&chars, text + 8, 8);
    else chars = reinterpret_cast<const std::uint16_t*>(text + 8);
    ip += 2 * game_function<Wcslen>(kWcslen)(chars) + 2;
    Value* slot = push(ctx);
    addref(s);
    addref(s);
    release_old(slot);
    slot->type = kString;
    addref(s);
    slot->payload = reinterpret_cast<std::uintptr_t>(s);
    release(s);
    release(s);
    release(s);
}

// 0xb7: the top kept (re-boxed as the number it is) when true; when false
// the expression is over with int 0 and the stack holds that alone.
void stop_unless(Context* ctx, const std::uint8_t*& ip) {
    Value* top = pop(ctx);
    const double x = number(top);
    std::int32_t i = truncate(x);
    std::uint32_t bits = static_cast<std::uint32_t>(i);
    std::uint32_t type = kInt;
    if (!(static_cast<double>(i) == x)) {
        const float f = narrow(x);
        bits = float_bits(f);
        i = truncate(f);
        type = kFloat;
    }
    if (i != 0) {
        push(ctx);
        release_old(top);
        top->type = type;
        top->payload = bits;  // all 8 bytes: the high half 0
        return;
    }
    ctx->sp = ctx->stack + 1;
    put_int(&ctx->stack[0], 0);
    ip = nullptr;
}

}  // namespace

DECOMP_LEAF Value* evaluate(Value* out, std::uint8_t* machine, Context* ctx, const std::uint8_t* ip) {
    g_evaluations.fetch_add(1, std::memory_order_relaxed);
    if (void* env = *reinterpret_cast<void**>(machine + kMachineEnv)) {
        std::uint64_t talk = g_talk_vtable.load(std::memory_order_relaxed);
        if (!talk) g_talk_vtable.store(talk = game_address(kTalkEnvVtable), std::memory_order_relaxed);
        if (reinterpret_cast<std::uint64_t>(*static_cast<void**>(env)) == talk)
            g_talk_evaluations.fetch_add(1, std::memory_order_relaxed);
    }
    while (ip) {
        const std::uint8_t op = *ip++;
        if (op == 0xa1 || op == 0xbb) break;
        if (op < 0x80) {
            put_int(push(ctx), static_cast<std::uint32_t>(static_cast<std::int32_t>(op) - 0x40));
            continue;
        }
        switch (op) {
        case 0x80: {
            float f;
            std::memcpy(&f, ip, 4);
            const std::int32_t i = truncate(f);
            Value* slot = push(ctx);
            if (static_cast<double>(i) == widen(f)) put_int(slot, static_cast<std::uint32_t>(i));
            else {
                release_old(slot);
                slot->type = kFloat;
                write_low32(slot, float_bits(f));
            }
            ip += 4;
            break;
        }
        case 0x81: {
            double d;
            std::memcpy(&d, ip, 8);
            put_number(push(ctx), d);
            ip += 8;
            break;
        }
        case 0x82: {
            std::uint32_t i;
            std::memcpy(&i, ip, 4);
            put_int(push(ctx), i);
            ip += 4;
            break;
        }
        case 0x83: table_read(machine, ctx, kMachineVariables); break;
        case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8a: case 0x8b:
            call(machine, ctx, op - 0x84);
            break;
        case 0x8c: binary(ctx, [](Value* s, double a, double b) { put_number(s, add_sd(b, a)); }); break;
        case 0x8d: {
            Value* top = pop(ctx);
            const double x = number(top);
            push(ctx);
            put_number(top, -x);
            break;
        }
        case 0x8e: binary(ctx, [](Value* s, double a, double b) { put_number(s, sub_sd(a, b)); }); break;
        case 0x8f: binary(ctx, [](Value* s, double a, double b) { put_number(s, mul_sd(b, a)); }); break;
        case 0x90: binary(ctx, [](Value* s, double a, double b) { put_number(s, div_sd(a, b)); }); break;
        case 0x91: binary(ctx, [](Value* s, double a, double b) { put_int(s, b > a); }); break;
        case 0x92: binary(ctx, [](Value* s, double a, double b) { put_int(s, a > b); }); break;
        case 0x93: binary(ctx, [](Value* s, double a, double b) { put_int(s, b >= a); }); break;
        case 0x94: binary(ctx, [](Value* s, double a, double b) { put_int(s, a >= b); }); break;
        case 0x95: binary(ctx, [](Value* s, double a, double b) { put_int(s, a == b); }); break;
        case 0x96: binary(ctx, [](Value* s, double a, double b) { put_int(s, a != b); }); break;
        case 0x97: {
            Value* top = pop(ctx);
            const double x = number(top);
            push(ctx);
            put_number(top, abs_bits(x));
            break;
        }
        case 0x98:
        case 0x99: {
            // Truth is "not 0.0" (NaN is true); an operand that is not a
            // number is false.
            const Value* b = pop(ctx);
            const double vb = number(b);
            Value* a = pop(ctx);
            const bool a_number = a->type == kFloat || a->type == kInt;
            const bool ta = a_number && !(number(a) == 0.0);
            const bool tb = !(vb == 0.0);
            push(ctx);
            put_int(a, op == 0x98 ? (ta && tb) : (ta || tb));
            break;
        }
        case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: case 0xa0: {
            Value* top = pop(ctx);
            const double x = number(top);
            push(ctx);
            bool r;
            switch (op) {
            case 0x9b: r = 0.0 > x; break;
            case 0x9c: r = x > 0.0; break;
            case 0x9d: r = 0.0 >= x; break;
            case 0x9e: r = x >= 0.0; break;
            case 0xa0: r = x != 0.0; break;
            default: r = x == 0.0; break;
            }
            put_int(top, r);
            break;
        }
        case 0xa2: case 0xa6: break;
        case 0xa3:
        case 0xa4: {
            Value local{0, 0, 0};
            game_function<Format>(kFormat)(&local, ctx, op == 0xa3 ? 2 : 3);
            assign(push(ctx), &local, 2);
            release_old(&local);
            break;
        }
        case 0xa5: string_literal(ctx, ip); break;
        case 0xa7: case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae:
            assign(&ctx->reg[op - 0xa7], ctx->sp - 1, 2);
            break;
        case 0xaf: case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: {
            const Value* r = &ctx->reg[op - 0xaf];
            assign(push(ctx), r, 3);
            break;
        }
        case 0xb7: stop_unless(ctx, ip); break;
        case 0xb8: table_read(machine, ctx, kMachineArguments); break;
        case 0xb9: {
            std::uint8_t* child = *reinterpret_cast<std::uint8_t**>(machine + kMachineChild);
            if (child) {
                assign(push(ctx), reinterpret_cast<const Value*>(child + kChildReturn), 3);
            } else {
                Value* slot = push(ctx);
                release_old(slot);
                slot->type = kInt;
                slot->payload = 0x7fffffff;  // all 8 bytes
            }
            break;
        }
        case 0xba: put_int(push(ctx), 0x7fffffff); break;
        default: {
            // 0xbc-0xff: the game's own handler.
            const auto* table = reinterpret_cast<const Handler*>(static_cast<std::uintptr_t>(game_address(kHandlers)));
            table[op](machine, ctx, &ip);
            break;
        }
        }
    }
    const Value* top = pop(ctx);
    out->type = top->type;
    if (top->type == kString) addref(string_of(top->payload));
    out->payload = top->payload;
    return out;
}

}  // namespace talk_script

// ---- Compare runs (BBHOST_DECOMP_COMPARE=1) ----
//
// The game's evaluator runs on the machine's context as ever, its
// environment's answers recorded on the way (a proxy in the machine's
// environment slot); ours then runs on a copy of the context as it was at
// entry - every string in it counted once more, so the copy owns what it
// holds - with the recording played back, so the environment is asked once.
// Compared: the result, the registers, the stack and its pointer, and the
// calls ours made against the game's.

namespace talk_script {
namespace {

void* g_game = nullptr;  // the trampoline into the game's evaluator
DecompCompare g_cmp;
std::atomic<std::uint64_t> g_calls_replayed{0}, g_uncompared{0};
thread_local int t_depth = 0;

constexpr std::size_t kMaxRecorded = 64;

struct Recorded {
    std::int32_t id = 0, count = 0;
    Value args[8]{};  // as the call carried them, its strings counted
    Value answer{};   // the environment's, a string counted
};

struct Proxy {
    void** vtable;
    void* real;
    std::vector<Recorded>* log;
    std::size_t next;
    bool mismatch, overflow;
};

void hold(const Value& v) {
    if (v.type == kString) addref(string_of(v.payload));
}
void drop(const Value& v) {
    if (v.type == kString) release(string_of(v.payload));
}

// A string's text: its 16-bit characters and their count.
std::u16string text_of(String* s) {
    const std::uint8_t* t = s->text;  // the 16-bit string at +0x10
    std::uint64_t size = 0, capacity = 0;
    std::memcpy(&size, t + 0x18, 8);
    std::memcpy(&capacity, t + 0x20, 8);
    const char16_t* chars;
    if (capacity >= 8) std::memcpy(&chars, t + 8, 8);
    else chars = reinterpret_cast<const char16_t*>(t + 8);
    return size < 0x10000 ? std::u16string(chars, size) : std::u16string(u"<too long>");
}

bool same(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    if (a.type == kString) return text_of(string_of(a.payload)) == text_of(string_of(b.payload));
    return static_cast<std::uint32_t>(a.payload) == static_cast<std::uint32_t>(b.payload);
}

GUEST_ABI Value* record(Value* r, Proxy* self, Call* c, std::uint8_t* machine) {
    reinterpret_cast<Query>((*static_cast<void***>(self->real))[2])(r, self->real, c, machine);
    if (self->log->size() >= kMaxRecorded || c->count < 1 || c->count > 8) {
        self->overflow = true;
        return r;
    }
    Recorded rec;
    rec.id = as_int(&c->v[0]);
    rec.count = c->count;
    for (int i = 0; i < c->count; ++i) {
        rec.args[i] = c->v[i];
        hold(rec.args[i]);
    }
    rec.answer = *r;
    hold(rec.answer);
    self->log->push_back(rec);
    return r;
}

GUEST_ABI Value* replay(Value* r, Proxy* self, Call* c, std::uint8_t*) {
    if (self->next >= self->log->size()) {
        self->mismatch = true;
        r->payload = 0;
        r->type = kFloat;
        return r;
    }
    const Recorded& rec = (*self->log)[self->next++];
    if (rec.count != c->count || rec.id != as_int(&c->v[0])) self->mismatch = true;
    for (int i = 1; i < rec.count && i < c->count; ++i)
        if (!same(rec.args[i], c->v[i])) self->mismatch = true;
    *r = rec.answer;
    hold(*r);  // the evaluator releases what it was given
    return r;
}

// The environments the compared expressions ran under, by their vtables:
// what a session reached (talk, the menus, others).
struct EnvCount {
    std::atomic<std::uint64_t> vtable{0}, calls{0};
};
EnvCount g_envs[16];

void count_env(void* env) {
    const std::uint64_t vt = env ? reinterpret_cast<std::uint64_t>(*static_cast<void**>(env)) : 0;
    for (EnvCount& e : g_envs) {
        std::uint64_t have = e.vtable.load(std::memory_order_relaxed);
        if (have == 0 && e.vtable.compare_exchange_strong(have, vt ? vt : 1)) have = vt ? vt : 1;
        if (have == (vt ? vt : 1)) {
            e.calls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void* g_record_vtable[3] = {nullptr, nullptr, reinterpret_cast<void*>(&record)};
void* g_replay_vtable[3] = {nullptr, nullptr, reinterpret_cast<void*>(&replay)};

void differ(const char* what, const std::uint8_t* ip, int index) {
    if (g_cmp.differ.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    char bytes[3 * 16 + 1] = {};
    for (int i = 0; i < 16 && ip; ++i) std::snprintf(bytes + 3 * i, 4, "%02x ", ip[i]);
    host_log("decomp: sub_2b73910 differs (%s %d) on the expression %s", what, index, bytes);
}

GUEST_ABI Value* compare(Value* out, std::uint8_t* machine, Context* ctx, const std::uint8_t* ip) {
    using Evaluate = GUEST_ABI Value* (*)(Value*, std::uint8_t*, Context*, const std::uint8_t*);
    const auto theirs = reinterpret_cast<Evaluate>(g_game);
    if (!tls_gs_mode() || t_depth > 0) {
        g_uncompared.fetch_add(1, std::memory_order_relaxed);
        return theirs(out, machine, ctx, ip);
    }
    ++t_depth;
    // The copy, owning what it holds.
    Context copy;
    std::memcpy(&copy, ctx, sizeof copy);
    copy.sp = copy.stack + (ctx->sp - ctx->stack);
    for (const Value& v : copy.stack) hold(v);
    for (const Value& v : copy.reg) hold(v);

    void** env_slot = reinterpret_cast<void**>(machine + kMachineEnv);
    void* env = *env_slot;
    count_env(env);
    std::vector<Recorded> log;
    log.reserve(8);
    Proxy rec{g_record_vtable, env, &log, 0, false, false};
    if (env) *env_slot = &rec;
    theirs(out, machine, ctx, ip);
    *env_slot = env;

    Value ours{0, 0, 0};
    Proxy rep{g_replay_vtable, env, &log, 0, false, false};
    if (env) *env_slot = &rep;
    evaluate(&ours, machine, &copy, ip);
    *env_slot = env;

    if (rec.overflow) {
        g_uncompared.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_cmp.calls.fetch_add(1, std::memory_order_relaxed);
        g_calls_replayed.fetch_add(log.size(), std::memory_order_relaxed);
        if (!same(*out, ours)) differ("result", ip, 0);
        else if (rep.mismatch || rep.next != log.size()) differ("calls", ip, static_cast<int>(rep.next));
        else if (copy.sp - copy.stack != ctx->sp - ctx->stack) differ("stack pointer", ip, static_cast<int>(copy.sp - copy.stack));
        else {
            for (int i = 0; i < 8; ++i)
                if (!same(copy.reg[i], ctx->reg[i])) {
                    differ("register", ip, i);
                    goto done;
                }
            for (int i = 0; i < 64; ++i)
                if (!same(copy.stack[i], ctx->stack[i])) {
                    differ("stack slot", ip, i);
                    goto done;
                }
        }
    }
done:
    drop(ours);
    for (const Value& v : copy.stack) drop(v);
    for (const Value& v : copy.reg) drop(v);
    for (const Recorded& r : log) {
        for (int i = 0; i < r.count; ++i) drop(r.args[i]);
        drop(r.answer);
    }
    --t_depth;
    return out;
}

// push rbp; mov rbp, rsp; push r15, r14, r13, r12, rbx; sub rsp, 0x18.
constexpr std::uint8_t kEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                   0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};

void report() {
    if (!decomp_comparing()) {
        host_log("decomp: the EzState evaluator ran %llu expressions, %llu of them the talk scripts'",
                 static_cast<ull>(g_evaluations.load()), static_cast<ull>(g_talk_evaluations.load()));
        return;
    }
    host_log("decomp: sub_2b73910: %llu environment calls replayed, %llu expressions left uncompared (nested or over %zu calls)",
             static_cast<ull>(g_calls_replayed.load()), static_cast<ull>(g_uncompared.load()), kMaxRecorded);
    const std::uint64_t slide = game_address(kTalkEnvVtable) - kTalkEnvVtable;
    for (const EnvCount& e : g_envs) {
        const std::uint64_t vt = e.vtable.load();
        if (!vt) break;
        host_log("decomp: sub_2b73910: %llu compared under %s 0x%llx%s", static_cast<ull>(e.calls.load()),
                 vt == 1 ? "no environment" : "the environment of vtable", static_cast<ull>(vt == 1 ? 0 : vt - slide),
                 vt - slide == kTalkEnvVtable ? " (the talk scripts')" : "");
    }
}

}  // namespace
}  // namespace talk_script

void decomp_talk_evaluator_add() {
    using namespace talk_script;
    DecompFunction f{"sub_2b73910", "talk", 0x2b73910, kEntry, sizeof(kEntry), reinterpret_cast<void*>(&evaluate),
                     DecompKind::Leaf, &g_game, reinterpret_cast<void*>(&compare), &g_cmp};
    f.report = &report;
    decomp_add(f);
}
