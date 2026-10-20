// The engine's own identities for a shader's resources, from the shader
// binary itself.
//
// Each of the game's shader containers keeps, after its OrbShdr footer (and
// the embedded constant buffer the Gnm header sizes), the reflection the
// shader compiler wrote: one record per resource (textures, constant buffers,
// structured and read-write buffers: API slot, type, size, the constant
// buffer's members), per member (name, type, offset, size, array count), per
// sampler state (slot, name), per input and output attribute (name, semantic,
// semantic index), and a string table they point into. The Gnm header before
// the code lists the input usage slots: which API slot sits in which user-data
// register, or at which dword of the extended user-data table a register pair
// points to. The two together name every binding the translator resolves by a
// resource path: the path's register (or table dword) is a usage slot, the slot
// a reflection record.
//
// That is what a native GX layer binds by and what an HLSL emitter declares:
// `cbuffer cbSampleOffset` of 256 bytes holding `g_avSampleOffsets`, not "the V#
// at dword 28 of the table in s[2:3]".
#pragma once

#include "gcn/translate.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gcn {

struct ReflectedResource {
    std::uint32_t slot = 0;           // API slot: texture, constant-buffer or buffer register
    std::uint32_t type = 0;           // the record's type word: low byte the kind (kResource*), then two bytes it does not decode
    std::uint32_t size = 0;           // bytes: a constant buffer's size, a structured buffer's element stride
    std::uint32_t first_element = 0;  // a constant buffer's members in ShaderReflection::elements
    std::uint32_t elements = 0;
    std::string name;
};
// Resource kinds (the low byte of ReflectedResource::type) as the records use
// them, named by what they hold.
constexpr std::uint32_t kResourceTexture2D = 0x02, kResourceTexture3D = 0x03, kResourceTextureCube = 0x04, kResourceTextureMs = 0x08;
constexpr std::uint32_t kResourceStructured = 0x0a, kResourceConstantBuffer = 0x16;
struct ReflectedElement {
    std::uint32_t type = 0;  // the record's type word (scalar, vector or matrix class and shape; not decoded)
    std::uint32_t offset = 0, size = 0;  // bytes inside its constant buffer
    std::int32_t array = -1;             // element count of an array; -1 not an array
    std::string name, type_name;         // the member, and its struct type's name ("(no_name)" for none)
};
struct ReflectedSampler {
    std::uint32_t slot = 0;
    std::string name;
};
struct ReflectedAttribute {
    std::uint32_t type = 0;   // the record's type word
    std::uint32_t index = 0;  // the semantic index
    std::string name, semantic;
};
// A Gnm input usage slot: `type` what it holds (0 a T#, 1 an S#, 2 a constant
// buffer's V#, 0x13 / 0x15 / 0x16 a pointer to a table of T#s, S#s or V#s,
// 0x1b the pointer to the extended user-data table, others not decoded), `api_slot` the resource, sampler or buffer register, `start_reg`
// the user-data register it starts at - 16 and up being dword (start_reg - 16)
// of the extended user-data table - and `flags`, whose low bits size it.
struct UsageSlot {
    std::uint8_t type = 0, api_slot = 0, start_reg = 0, flags = 0;
};
constexpr std::uint8_t kUsageTexture = 0x00, kUsageSampler = 0x01, kUsageConstantBuffer = 0x02, kUsageExtendedUserData = 0x1b;
// Pointers to tables of descriptors indexed by API slot from the slot's own:
// T#s eight dwords each, S#s and V#s four (the material shaders' textures and
// samplers, and constant buffers).
constexpr std::uint8_t kUsageTextureTable = 0x13, kUsageSamplerTable = 0x15, kUsageConstantBufferTable = 0x16;
struct ShaderReflection {
    // The constant buffer the shader carries itself, after the OrbShdr footer
    // (bytes; 0 none): what constant-buffer slot 15 reads in every container
    // whose usage slots name slot 15, for which the reflection has no record.
    std::uint32_t embedded_cb_bytes = 0;
    std::vector<UsageSlot> usage;
    std::vector<ReflectedResource> resources;
    std::vector<ReflectedElement> elements;
    std::vector<ReflectedSampler> samplers;
    std::vector<ReflectedAttribute> inputs, outputs;
};
// Parses a vertex- or pixel-shader container (type 1 or 2: the stages whose
// usage table the Gnm header places where this reads it; the reflection
// itself parses for every type the bundles hold but the geometry shaders').
// False, with `error`, when the container has another layout.
bool parse_reflection(const std::vector<std::uint8_t>& container, ShaderReflection& out, std::string* error = nullptr);

// One binding of a translation, named by the engine: empty `name` where the
// containers do not reach it (a resource path through more than the extended
// user-data table, or a usage slot without a record).
struct BindingName {
    std::string name;
    int api_slot = -1;    // the usage slot's API slot
    int usage_type = -1;  // the usage slot's type
    std::uint32_t resource_type = 0;          // the reflection record's type word (resources)
    std::uint32_t size = 0;                   // a constant buffer's bytes
    std::vector<ReflectedElement> elements;   // a constant buffer's members
};
struct ShaderNames {
    std::vector<BindingName> images, samplers, buffers;  // in TranslateResult::images / samplers / buffers order
    // The reflection's attributes: a pixel shader's inputs in attribute order
    // (attr k), a vertex shader's in its input table's order (vertex input
    // location k); and the outputs.
    std::vector<ReflectedAttribute> inputs, outputs;
    std::size_t named = 0, bindings = 0;  // bindings named, of all
};
// The names of `paths`'s bindings (a translation of the container's program).
ShaderNames name_bindings(const ShaderReflection& reflection, const TranslateResult& paths);

// The engine's names on a lifted module: LiftResult-style variable names the
// lifter gives (img<k>, smp<k>, cb<k>, attr<k>, vertex_in<k>) replaced by the
// names `names` holds for them - OpName only, so the module computes exactly
// what it did.
std::vector<std::uint32_t> apply_names(const std::vector<std::uint32_t>& spirv, const ShaderNames& names);

}  // namespace gcn
