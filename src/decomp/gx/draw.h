// GX's immediate context as source (decomp/gx/draw.cpp): what the GX trace
// (hle/gx_trace.cpp) and the methods share.
#pragma once

#include <cstdint>

namespace decomp_gx {

// The GX trace's entry, gx_hook(kind, frame), behind the FS-switching thunk
// (thunk_wrap): what the prologue stubs called before these methods were
// ours, and now ours calls first. Set once by hle_gx_trace_arm, before the
// game draws; null when the trace and the GX backend are off, and ours is
// then the game's method alone. The frame is the stubs' - r9, r8, rcx, rdx,
// rsi, rdi, the return address, a stack argument - with a return slot below
// it; the kind is the trace's (these values, hle/gx_trace.cpp checks them).
extern void* g_gx_hook_entry;
enum MethodKind : std::uint64_t {
    kGxDraw = 0,
    kGxDrawIndexed = 1,
    kGxDrawInstanced = 2,
    kGxDrawIndexedInstanced = 3,
    kGxDrawInstancedIndirect = 4,
    kGxDrawIndexedInstancedIndirect = 5,
    kGxCommit = 6,
    kGxDispatch = 7,
    kGxDispatchIndirect = 8,
};
// The methods' names on the decomp list, by kind: hle_gx_trace_arm installs
// no stub over one that is placed (it would not find the game's prologue).
extern const char* const kMethodNames[9];

// What the GX trace takes for the caller of a Gnm writer it hooks: a return
// address in ours' methods (their code is a section of its own, the bounds
// below) is the game's return address from the same method's draw or
// dispatch writer - the address the trace knows a GX method's packet by (a
// draw's records go with it, a dispatch through GX's compute commit is
// one) - and any other address is itself. A guest address either way.
extern std::uint64_t g_method_code_lo, g_method_code_hi;
std::uint64_t game_return_in_methods(std::uint64_t ret);
inline std::uint64_t game_return(std::uint64_t ret) {
    return ret - g_method_code_lo < g_method_code_hi - g_method_code_lo ? game_return_in_methods(ret) : ret;
}

}  // namespace decomp_gx
