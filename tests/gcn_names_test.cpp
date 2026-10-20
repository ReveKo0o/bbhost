// The engine's names for a shader's bindings (gcn/names.h): every vertex- and
// pixel-shader container of the game's bundles has a reflection that parses -
// its sections exactly fill the container, every name inside the string table
// - and a few programs' bindings, translated as the renderer's no-fallback
// variant, carry the names their containers give: the SSAO pass's constant
// buffers through the extended user-data table (cbSampleOffset, the one read
// at a run-time offset, with its 256 bytes and g_avSampleOffsets), a decal's
// textures through a T# table, a forward shader's samplers through an S#
// table and its embedded constant buffer at slot 15, a vertex shader's inputs.
// Then the names on a lifted module, which must stay valid and compute the
// same (only OpName changes). Skips without the game's files (BBHOST_APP0).
#include "test_app0.h"
#include "gcn/container.h"
#include "gcn/lift.h"
#include "gcn/names.h"
#include "gcn/translate.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

namespace {

std::map<std::string, std::vector<std::uint8_t>> load(const std::string& bundle) {
    std::map<std::string, std::vector<std::uint8_t>> out;
    std::vector<std::uint8_t> raw;
    if (!gcn::read_file(bundle, raw)) return out;
    std::string err;
    std::vector<gcn::BundleEntry> entries;
    if (!gcn::bnd4_entries(gcn::dcx_decompress(raw, &err), entries, &err)) return out;
    for (gcn::BundleEntry& e : entries) out[e.name] = std::move(e.data);
    return out;
}

// The program's bindings as the renderer's no-fallback variant names them.
struct Named {
    gcn::TranslateResult paths;
    gcn::ShaderNames names;
};
Named named(const std::vector<std::uint8_t>& c) {
    Named n;
    gcn::ShaderCode code;
    CHECK(gcn::shader_code(c, code));
    std::size_t shdr = 0;
    while (shdr + 0x60 <= c.size() && std::memcmp(c.data() + shdr, "Shdr", 4) != 0) ++shdr;
    std::uint32_t regs[16];
    std::memcpy(regs, c.data() + shdr + 16, sizeof(regs));
    gcn::TranslateOptions o;
    o.stage = code.type == 1 ? gcn::Stage::Vertex : gcn::Stage::Pixel;
    o.rsrc1 = regs[4];
    o.rsrc2 = regs[5];
    o.ps_input_ena = code.type == 2 ? regs[8] : 0;
    o.cb_ssbo = true;
    o.cb_no_fallback = true;
    const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
    n.paths = gcn::translate(p, o);
    gcn::ShaderReflection r;
    std::string err;
    CHECK(gcn::parse_reflection(c, r, &err));
    n.names = gcn::name_bindings(r, n.paths);
    return n;
}

std::string image_named(const Named& n, const char* path) {
    for (std::size_t k = 0; k < n.paths.images.size(); ++k) {
        if (n.paths.images[k].path.str() == path) return n.names.images[k].name;
    }
    return "(no such binding)";
}
const gcn::BindingName* buffer_named(const Named& n, const char* path) {
    for (std::size_t k = 0; k < n.paths.buffers.size(); ++k) {
        if (n.paths.buffers[k].path.str() == path) return &n.names.buffers[k];
    }
    return nullptr;
}
std::string sampler_named(const Named& n, const char* path) {
    for (std::size_t k = 0; k < n.paths.samplers.size(); ++k) {
        if (n.paths.samplers[k].path.str() == path) return n.names.samplers[k].name;
    }
    return "(no such binding)";
}

}  // namespace

int main() {
    const std::string dir = test_app0_file("dvdroot_ps4/shader/");
    if (dir.empty()) {
        std::puts("gcn_names_test: skipped (BBHOST_APP0 not set)");
        return kTestSkip;
    }
    // Every vertex and pixel shader's reflection parses.
    std::size_t parsed = 0, containers = 0;
    std::map<std::string, std::map<std::string, std::vector<std::uint8_t>>> bundles;
    for (const char* b : {"gxdecal.shaderbnd", "gxffxshader.shaderbnd", "gxflvershader.shaderbnd", "gxflvershader.shaderbnddebug", "gxgui.shaderbnd",
                          "gxposteffect.shaderbnd", "gxrenderershader.shaderbnd", "gxshader.shaderbnd"}) {
        bundles[b] = load(dir + b + ".dcx");
        if (bundles[b].empty()) {
            std::printf("gcn_names_test: skipped (%s.dcx not readable)\n", b);
            return kTestSkip;
        }
        for (const auto& [name, c] : bundles[b]) {
            gcn::ShaderCode code;
            if (!gcn::shader_code(c, code) || (code.type != 1 && code.type != 2)) continue;
            ++containers;
            gcn::ShaderReflection r;
            std::string err;
            if (gcn::parse_reflection(c, r, &err)) {
                ++parsed;
            } else {
                std::fprintf(stderr, "%s: %s\n", name.c_str(), err.c_str());
            }
        }
    }
    CHECK(containers > 13000 && parsed == containers);

    // The SSAO pass: textures and samplers in user data and the table, the
    // constant buffers through the extended user-data table.
    {
        const Named n = named(bundles["gxposteffect.shaderbnd"].at("ScreenSpaceAO2_WithNormal.ppo"));
        CHECK(image_named(n, "user_sgpr[4]") == "g_DepthTex");
        CHECK(image_named(n, "user_sgpr[2]->load(+0)") == "g_NormalTex");
        CHECK(image_named(n, "user_sgpr[2]->load(+32)") == "g_RandomRotationTex");
        CHECK(sampler_named(n, "user_sgpr[12]") == "SS_WrapPoint");
        const gcn::BindingName* offsets = buffer_named(n, "user_sgpr[2]->load(+112)");
        CHECK(offsets && offsets->name == "cbSampleOffset" && offsets->size == 256 && offsets->api_slot == 1 && offsets->elements.size() == 1 &&
              offsets->elements[0].name == "g_avSampleOffsets" && offsets->elements[0].size == 256);
        const gcn::BindingName* param2 = buffer_named(n, "user_sgpr[2]->load(+128)");
        CHECK(param2 && param2->name == "cbSSAOParam2" && param2->size == 272 && param2->elements.size() == 8);
        const gcn::BindingName* table = buffer_named(n, "user_sgpr[2]");
        CHECK(table && table->name == "(extended user data)");
        CHECK(n.names.inputs.size() == 1 && n.names.inputs[0].name == "In.vScreenPosition" && n.names.inputs[0].semantic == "TEXCOORD");
        CHECK(n.names.named == n.names.bindings);
    }
    // A decal: its textures through a table of T#s, eight dwords a slot.
    {
        const Named n = named(bundles["gxdecal.shaderbnd"].at("Lightprepass0\\Decal_Blood_DifSpcBump.ppo"));
        CHECK(image_named(n, "user_sgpr[12]->load(+0)") == "g_DiffuseTexture");
        CHECK(image_named(n, "user_sgpr[12]->load(+128)") == "g_DepthTexture");
        CHECK(image_named(n, "user_sgpr[12]->load(+448)") == "g_GBuffer1Texture");
        const gcn::BindingName* table = buffer_named(n, "user_sgpr[12]");
        CHECK(table && table->name == "(texture table)");
        CHECK(n.names.named == n.names.bindings);
    }
    // A forward material shader: samplers through a table of S#s, and the
    // dither texture the alpha test reads.
    {
        const Named n = named(bundles["gxflvershader.shaderbnddebug"].at("GXFlver_ColDifSpcBumpDirtMaskEnvDOL_FwdSdwDbg.ppo"));
        CHECK(image_named(n, "user_sgpr[12]->load(+1216)") == "g_DitherTexture");
        CHECK(sampler_named(n, "user_sgpr[14]->load(+192)") == "SS_Shadow");
        CHECK(n.names.named == n.names.bindings);
    }
    // A debug shader's embedded constant buffer: slot 15, no record.
    {
        const Named n = named(bundles["gxflvershader.shaderbnddebug"].at("GXFlver_Col_FwdDbg.ppo"));
        const gcn::BindingName* embedded = buffer_named(n, "user_sgpr[2]->load(+128)");
        CHECK(embedded && embedded->name == "(embedded constant buffer)" && embedded->api_slot == 15 && embedded->size > 0);
    }
    // A vertex shader's inputs in its input table's order.
    {
        const Named n = named(bundles["gxflvershader.shaderbnd"].at("GXFlver_Col_Dpt.vpo"));
        CHECK(!n.names.inputs.empty() && !n.names.inputs[0].semantic.empty());
        CHECK(n.names.named == n.names.bindings);
    }
    // On a lifted module: OpName only, still valid.
    {
        const std::vector<std::uint8_t>& c = bundles["gxposteffect.shaderbnd"].at("ScreenSpaceAO2_WithNormal.ppo");
        gcn::ShaderCode code;
        gcn::shader_code(c, code);
        std::size_t shdr = 0;
        while (std::memcmp(c.data() + shdr, "Shdr", 4) != 0) ++shdr;
        std::uint32_t regs[16];
        std::memcpy(regs, c.data() + shdr + 16, sizeof(regs));
        gcn::TranslateOptions o;
        o.stage = gcn::Stage::Pixel;
        o.rsrc1 = regs[4];
        o.rsrc2 = regs[5];
        o.ps_input_ena = regs[8];
        o.descriptor_set = 1;
        o.cb_ssbo = true;
        o.cb_no_fallback = true;
        const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
        const gcn::TranslateResult ref = gcn::translate(p, o);
        gcn::LiftResult l = gcn::lift_pixel_shader(p, o, ref);
        CHECK(l.ok());
        const std::vector<std::uint32_t> before = l.spirv;
        gcn::ShaderReflection r;
        CHECK(gcn::parse_reflection(c, r));
        gcn::name_lift(l, ref, r);
        CHECK(l.spirv != before && l.names.named == l.names.bindings);
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
        spvtools::ValidatorOptions vo;
        vo.SetAllowOffsetTextureOperand(true);
        CHECK(tools.Validate(l.spirv.data(), l.spirv.size(), vo));
        // Everything but the debug names is the same module: strip OpName (5) from both.
        const auto strip = [](const std::vector<std::uint32_t>& m) {
            std::vector<std::uint32_t> out(m.begin(), m.begin() + 5);
            for (std::size_t i = 5; i < m.size();) {
                const std::uint32_t n = m[i] >> 16;
                if ((m[i] & 0xffff) != 5) out.insert(out.end(), m.begin() + static_cast<std::ptrdiff_t>(i), m.begin() + static_cast<std::ptrdiff_t>(i + n));
                i += n;
            }
            return out;
        };
        CHECK(strip(l.spirv) == strip(before));
        std::string text;
        CHECK(tools.Disassemble(l.spirv, &text));
        CHECK(text.find("OpName %") != std::string::npos && text.find("\"cbSampleOffset\"") != std::string::npos &&
              text.find("\"g_DepthTex\"") != std::string::npos && text.find("\"In.vScreenPosition\"") != std::string::npos);
    }
    if (g_failures) return 1;
    std::printf("gcn_names_test ok: %zu vertex- and pixel-shader reflections parsed; the sampled programs' bindings carry their containers' names\n",
                parsed);
    return 0;
}
