// The talk scripts' expression evaluator (decomp/talk/evaluator.cpp): the
// values, context and call object it works on, shared with its test.
#pragma once

#include "guest_abi.h"

#include <cstddef>
#include <cstdint>

namespace talk_script {

// EzStateValue, 16 bytes: the payload - an int or float in its low half,
// or a string object - and its type. Copies keep all 8 payload bytes, as the
// game's do; ints and floats are written 4 bytes at a time, so the high half
// of their payload is whatever the slot held before.
struct Value {
    std::uint64_t payload;
    std::uint32_t type;
    std::uint32_t pad;
};
static_assert(sizeof(Value) == 0x10, "EzStateValue");

constexpr std::uint32_t kFloat = 1, kInt = 2, kString = 3;

// A counted string (0x48): slot 0 of its vtable destroys it, the count at
// +8; the text is a 16-bit string at +0x10 (its characters at +0x18 - in
// place while the capacity at +0x30 is under 8, else a pointer there - and
// its length at +0x28).
struct String {
    void** vtable;
    std::int32_t count;
    std::int32_t pad;
    std::uint8_t text[0x38];
};
static_assert(sizeof(String) == 0x48, "the evaluator's string");

// The evaluation context, a local of the machine's Update (0x490): the value
// stack, eight registers, the stack pointer (the next free slot) and the
// allocator new strings come from.
struct Context {
    Value stack[64];
    Value reg[8];
    Value* sp;
    void* allocator;
};
static_assert(offsetof(Context, reg) == 0x400 && offsetof(Context, sp) == 0x480 && offsetof(Context, allocator) == 0x488 &&
                  sizeof(Context) == 0x490,
              "the evaluation context");

// A function call as the environment sees it: the game's call class
// (vtable 0x56c8850: slot 2 the id, 3 the count, 4 a copy of value i), the
// count of values (arguments + 1), value 0 the function's id. The game's has
// room for seven values; ours for eight (decomp/talk/evaluator.cpp, 0x8b).
struct Call {
    void** vtable;
    std::int32_t count;
    std::int32_t pad;
    Value v[8];
};

// The machine fields the evaluator reads.
constexpr std::size_t kMachineVariables = 0x70;  // Value* (bank 4 writes them, 0x83 reads)
constexpr std::size_t kMachineChild = 0x90;      // the child machine (bank 6); its return value at +0xc8
constexpr std::size_t kMachineArguments = 0xa8;  // Value* (a child's call arguments, 0xb8 reads)
constexpr std::size_t kMachineEnv = 0x130;       // the environment: slot 2 answers a call
constexpr std::size_t kChildReturn = 0xc8;

// Ours, in the game's place: evaluate `ip` on `ctx` for `machine` into `out`.
GUEST_ABI Value* evaluate(Value* out, std::uint8_t* machine, Context* ctx, const std::uint8_t* ip);

}  // namespace talk_script
