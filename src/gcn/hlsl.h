// HLSL from a lifted shader (gcn/lift.h).
//
// The lifter's SPIR-V is the shader's IR of record: typed SSA in one function,
// structured selections and loops, a fixed set of globals. emit_hlsl prints
// that module as HLSL (Shader Model 6, -HV 2021) that DXC compiles back to
// SPIR-V for Vulkan with the same interface - descriptor sets and bindings,
// locations, built-ins, specialization constants - and the same computation,
// instruction for instruction where the result's bits could depend on it:
//
//   * every float value is `precise`, so DXC marks its arithmetic
//     NoContraction as the lift does (no fusing, no reassociation, no
//     folding by spirv-opt);
//   * an operation whose HLSL spelling DXC lowers to another instruction is
//     written as inline SPIR-V (vk::ext_instruction) with a readable name:
//     FSign (HLSL sign() returns an int), Ldexp (ldexp() is x * exp2(e)),
//     PackHalf2x16, FMin/FMax (min()/max() are NMin/NMax, which the lift
//     uses for v_min/v_max), bitfield extracts;
//   * discard stays OpKill: the helper-lane query is the HelperInvocation
//     built-in (vk::ext_builtin_input), not IsHelperLane(), which would turn
//     every discard into a demote;
//   * quad operations are QuadReadLaneAt / QuadReadAcross*, which DXC lowers
//     to the lift's OpGroupNonUniformQuadBroadcast / QuadSwap.
//
// Constant buffers are ByteAddressBuffers read a dword at a time, as the lift
// reads them; the params block is a ConstantBuffer with the lift's offsets.
// Values keep the module's ids (v123), so the HLSL can be read beside the
// SPIR-V; resources keep the names the module gives them (the engine's,
// after gcn::name_lift).
//
// Provenance: the source is derived from the game's shaders. It is generated
// on the player's machine and never shipped or committed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gcn {

struct HlslOptions {
    std::string title;  // the program's name, for the header comment
};

struct HlslResult {
    std::string source;
    std::string profile;                  // ps_6_6, vs_6_6, hs_6_6, ds_6_6
    std::string entry = "main";
    std::vector<std::string> arguments;   // DXC arguments this module needs (-enable-16bit-types)
    std::vector<std::string> errors;      // what the printer does not take: empty on success
    bool ok() const { return errors.empty() && !source.empty(); }
};

// Prints a lifted module as HLSL. A module outside the subset the lifter
// writes comes back with `errors` (its first unsupported construct) and the
// caller keeps the lifted SPIR-V.
HlslResult emit_hlsl(const std::vector<std::uint32_t>& spirv, const HlslOptions& options = {});

// Whether two modules declare the same interface: execution model and modes,
// descriptor bindings (set, binding, kind, image shape and texel type,
// buffer layout), stage inputs and outputs (location or built-in, type,
// interpolation), and specialization constant ids. Empty when they do, else
// the first difference.
std::string hlsl_interface_diff(const std::vector<std::uint32_t>& lifted, const std::vector<std::uint32_t>& compiled);

// Whether DXC kept the lift's operations where their results' bits depend on
// which instruction computes them: no float, image, quad, derivative or
// discard operation (by opcode, and by GLSL.std.450 number) that the lift
// does not use, none more often than the lift uses it (DXC may merge and drop
// them, not add them; GetDimensions' size and level queries and
// CalculateLevelOfDetail excepted), and every float add, subtract, multiply,
// divide and negate (of something other than a literal) NoContraction. Empty
// when it did, else the first difference.
std::string hlsl_operation_diff(const std::vector<std::uint32_t>& lifted, const std::vector<std::uint32_t>& compiled);

}  // namespace gcn
