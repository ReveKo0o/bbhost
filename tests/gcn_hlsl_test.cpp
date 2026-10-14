// The HLSL printer (gcn/hlsl.h) on hand-encoded programs the lifter takes:
// quad swizzles in every quad pattern, v_cndmask_b32 with source modifiers, a
// sample and a LOD query in a divergent region, a sample's run-time offset
// (both ways the lift takes it), loops with per-lane exits
// (one inside a kill region, with the helper-lane query), a depth export and
// image loads and sizes. Each lift is printed, the source checked for the
// spellings that keep its instructions, and - where DXC is installed (host/
// dxc.h) - compiled: valid SPIR-V with the lift's interface and operations.
// Synthetic programs only: no game shader is read.
#include "gcn/hlsl.h"
#include "gcn/isa.h"
#include "gcn/lift.h"
#include "gcn/translate.h"
#include "host/dxc.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

namespace {

gpu::Dxc g_dxc;
bool g_have_dxc = false;
int g_compiled = 0, g_printed = 0;

bool spirv_valid(const std::vector<std::uint32_t>& words, std::string& msg) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) {
        if (msg.empty()) msg = m;
    });
    spvtools::ValidatorOptions vo;
    vo.SetAllowOffsetTextureOperand(true);
    return tools.Validate(words.data(), words.size(), vo);
}

// Prints `lift`, checks the source holds each of `spellings`, and compiles it.
// `runtime_offset`: whether the module samples with a run-time offset, which
// DXC compiles without its own validator (HlslResult::validate_offsets).
void through_hlsl(const gcn::LiftResult& lift, const std::string& what, const std::vector<std::string>& spellings = {}, bool runtime_offset = false) {
    CHECK(lift.ok());
    if (!lift.ok()) {
        std::fprintf(stderr, "%s: not lifted: %s\n", what.c_str(), lift.rejections.empty() ? "?" : lift.rejections[0].c_str());
        return;
    }
    gcn::HlslOptions o;
    o.title = what;
    const gcn::HlslResult h = gcn::emit_hlsl(lift.spirv, o);
    CHECK(h.ok());
    if (!h.ok()) {
        std::fprintf(stderr, "%s: not printed: %s\n", what.c_str(), h.errors.empty() ? "?" : h.errors[0].c_str());
        return;
    }
    ++g_printed;
    CHECK(h.validate_offsets == runtime_offset);
    for (const std::string& s : spellings) {
        const bool found = h.source.find(s) != std::string::npos;
        CHECK(found);
        if (!found) std::fprintf(stderr, "%s: the HLSL has no \"%s\"\n", what.c_str(), s.c_str());
    }
    if (!g_have_dxc) return;
    gpu::DxcOptions dopt;
    std::vector<std::uint32_t> spirv;
    std::string error;
    const bool compiled = g_dxc.compile(h, dopt, spirv, error);
    CHECK(compiled);
    if (!compiled) {
        std::fprintf(stderr, "%s: DXC: %s\n%s\n", what.c_str(), error.c_str(), h.source.c_str());
        return;
    }
    std::string msg;
    const bool valid = spirv_valid(spirv, msg);
    CHECK(valid);
    if (!valid) std::fprintf(stderr, "%s: DXC's module is invalid: %s\n", what.c_str(), msg.c_str());
    const std::string iface = gcn::hlsl_interface_diff(lift.spirv, spirv), ops = gcn::hlsl_operation_diff(lift.spirv, spirv);
    CHECK(iface.empty());
    CHECK(ops.empty());
    if (!iface.empty()) std::fprintf(stderr, "%s: interface: %s\n", what.c_str(), iface.c_str());
    if (!ops.empty()) std::fprintf(stderr, "%s: operations: %s\n", what.c_str(), ops.c_str());
    ++g_compiled;
}

// ---- Sea Islands encodings ---------------------------------------------------------
constexpr std::uint32_t kVcc = 106, kExec = 126, kZero = 128, kHalf = 240, kOne = 242, kV = 256, kEndpgm = 0xbf810000u;
constexpr std::uint32_t kGetLod = 96, kSampleLz = 39, kSampleLzO = 55;

struct Asm {
    std::vector<std::uint32_t> w;
    void sop1(std::uint32_t op, std::uint32_t sdst, std::uint32_t ssrc0) { w.push_back(0xbe800000u | sdst << 16 | op << 8 | ssrc0); }
    void sop2(std::uint32_t op, std::uint32_t sdst, std::uint32_t ssrc0, std::uint32_t ssrc1) {
        w.push_back(0x80000000u | op << 23 | sdst << 16 | ssrc1 << 8 | ssrc0);
    }
    void sopc(std::uint32_t op, std::uint32_t ssrc0, std::uint32_t ssrc1) { w.push_back(0xbf000000u | op << 16 | ssrc1 << 8 | ssrc0); }
    void vop1(std::uint32_t op, std::uint32_t vdst, std::uint32_t src0) { w.push_back(0x7e000000u | vdst << 17 | op << 9 | src0); }
    void vop2(std::uint32_t op, std::uint32_t vdst, std::uint32_t src0, std::uint32_t vsrc1) { w.push_back(op << 25 | vdst << 17 | vsrc1 << 9 | src0); }
    void vopc(std::uint32_t op, std::uint32_t src0, std::uint32_t vsrc1) { w.push_back(0x7c000000u | op << 17 | vsrc1 << 9 | src0); }
    void interp(std::uint32_t vdst, std::uint32_t attr, std::uint32_t chan) {
        w.push_back(0xc8000000u | vdst << 18 | attr << 10 | chan << 8 | 0u);
        w.push_back(0xc8000000u | vdst << 18 | 1u << 16 | attr << 10 | chan << 8 | 1u);
    }
    void mimg(std::uint32_t op, std::uint32_t vdata, std::uint32_t vaddr, std::uint32_t dmask) {
        w.push_back(0xf0000000u | op << 18 | dmask << 8);
        w.push_back(vaddr | vdata << 8 | (4u / 4) << 16 | (12u / 4) << 21);  // T# in s[4:11], S# in s[12:15]
    }
    void swizzle(std::uint32_t vdst, std::uint32_t vaddr, std::uint32_t off) {  // ds_swizzle_b32
        w.push_back(0xd8000000u | 53u << 18 | off);
        w.push_back(vdst << 24 | vaddr);
    }
    void wqm() { sop1(10, kExec, kExec); }
    void exp_mrt0(std::uint32_t v) {  // exp mrt0, v..v+3 done vm
        w.push_back(0xf800180fu);
        w.push_back(v | (v + 1) << 8 | (v + 2) << 16 | (v + 3) << 24);
    }
    std::size_t branch(std::uint32_t op) {
        w.push_back(0xbf800000u | op << 16);
        return w.size() - 1;
    }
    void land(std::size_t at) { w[at] |= static_cast<std::uint16_t>(w.size() - at - 1); }
    void back(std::uint32_t op, std::size_t target) {
        w.push_back(0xbf800000u | op << 16 | static_cast<std::uint16_t>(static_cast<int>(target) - static_cast<int>(w.size()) - 1));
    }
};

gcn::LiftResult lift_ps(const std::vector<std::uint32_t>& words, std::uint32_t dim = 1) {
    const gcn::Program p = gcn::decode(words.data(), words.size());
    CHECK(p.errors.empty());
    gcn::TranslateOptions o;
    o.stage = gcn::Stage::Pixel;
    o.rsrc2 = 16u << 1;  // s0-s15 user data
    o.ps_input_ena = 2;  // v0, v1: the perspective centre
    o.descriptor_set = 1;
    o.cb_ssbo = true;
    o.cb_no_fallback = true;
    o.image_dims = {{dim, false}};
    const gcn::TranslateResult ref = gcn::translate(p, o);
    CHECK(ref.ok());
    return gcn::lift_pixel_shader(p, o, ref);
}

// ---- the programs ------------------------------------------------------------------
void swizzles() {
    // Whole-quad mode, v2 = attr0.x, v3 = a quad-mode swizzle of it, exported.
    int lifted = 0;
    for (std::uint32_t pattern = 0; pattern < 256; ++pattern) {
        Asm a;
        a.sop1(4, 2, kExec);
        a.wqm();
        a.interp(2, 0, 0);
        a.swizzle(3, 2, 0x8000u | pattern);
        a.w.push_back(0xbf8c007fu);  // s_waitcnt lgkmcnt(0)
        a.sop1(4, kExec, 2);
        a.exp_mrt0(3);
        a.w.push_back(kEndpgm);
        const gcn::LiftResult l = lift_ps(a.w);
        if (!l.ok()) continue;
        ++lifted;
        std::vector<std::string> spell;
        if (pattern == 0x55) spell = {"QuadReadLaneAt("};
        if (pattern == 0xb1) spell = {"QuadReadAcrossX("};
        if (pattern == 0x4e) spell = {"QuadReadAcrossY("};
        if (pattern == 0x1b) spell = {"QuadReadAcrossDiagonal("};
        through_hlsl(l, "quad pattern " + std::to_string(pattern), spell);
    }
    CHECK(lifted == 256);
}

void cndmask() {
    // x = attr0.x; vcc = 0 < x; v3 = vcc ? src1 : src0, with abs and neg on the sources.
    for (std::uint32_t abs = 0; abs < 4; ++abs) {
        for (std::uint32_t neg = 0; neg < 4; ++neg) {
            Asm a;
            a.interp(2, 0, 0);
            a.vopc(1 /* v_cmp_lt_f32 */, kZero, 2);
            a.w.push_back(0xd2000000u | abs << 8 | 3u);
            a.w.push_back(neg << 29 | kVcc << 18 | (kV + 2) << 9 | (kV + 2));
            a.exp_mrt0(3);
            a.w.push_back(kEndpgm);
            const gcn::LiftResult l = lift_ps(a.w);
            if (l.ok()) through_hlsl(l, "v_cndmask_b32 abs " + std::to_string(abs) + " neg " + std::to_string(neg), {"select("});
        }
    }
}

void samples_and_loops() {
    // A LOD query on coordinates computed in whole-quad mode inside a divergent region.
    {
        Asm a;
        a.sop1(4, 20, kExec);
        a.wqm();
        a.interp(2, 0, 0);
        a.interp(3, 0, 1);
        a.vopc(1, kZero, 2);   // v_cmp_lt_f32 vcc, 0, v2
        a.sop1(36, 22, kVcc);  // s_and_saveexec_b64
        const std::size_t skip = a.branch(8);
        a.sop1(4, kVcc, kExec);
        a.wqm();
        a.vop2(8, 8, kHalf, 2);  // v_mul_f32 v8, 0.5, v2
        a.vop2(8, 9, kHalf, 3);
        a.sop1(4, kExec, kVcc);
        a.mimg(kGetLod, 4, 8, 0x2);
        a.land(skip);
        a.sop1(4, kExec, 22);
        a.exp_mrt0(4);
        a.w.push_back(kEndpgm);
        through_hlsl(lift_ps(a.w), "image_get_lod in a divergent region", {"CalculateLevelOfDetailUnclamped(", "precise float"});
    }
    // A sample at level 0 with whole-quad coordinates.
    {
        Asm a;
        a.sop1(4, 20, kExec);
        a.wqm();
        a.interp(2, 0, 0);
        a.interp(3, 0, 1);
        a.mimg(kSampleLz, 4, 2, 0xf);
        a.sop1(4, kExec, 20);
        a.exp_mrt0(4);
        a.w.push_back(kEndpgm);
        through_hlsl(lift_ps(a.w), "image_sample_lz", {"SampleLevel("});
    }
    // A sample whose offset is a run-time value: the Offset operand where the
    // device has maintenance8 (DXC's validator refuses it: -Vd, and the caller
    // validates), the coordinates moved by it elsewhere.
    for (const bool runtime : {false, true}) {
        Asm a;
        a.sop1(4, 20, kExec);
        a.wqm();
        a.interp(2, 0, 0);
        a.interp(3, 0, 1);
        a.vop1(8, 8, kV + 3);  // v8 = int(attr0.y): the packed offsets
        a.vop1(1, 9, kV + 2);
        a.vop1(1, 10, kV + 3);
        a.mimg(kSampleLzO, 4, 8, 0xf);
        a.sop1(4, kExec, 20);
        a.exp_mrt0(4);
        a.w.push_back(kEndpgm);
        gcn::set_runtime_sample_offsets(runtime);
        through_hlsl(lift_ps(a.w), std::string("image_sample_lz_o, ") + (runtime ? "run-time offset" : "moved coordinates"),
                     runtime ? std::vector<std::string>{"SampleLevel("} : std::vector<std::string>{"SampleLevel(", "GetDimensions("}, runtime);
        gcn::set_runtime_sample_offsets(false);
    }
    // A uniform loop with a kill region before it: the loop's header phis
    // (declared at the top), its exit, and the values used past it.
    {
        Asm a;
        a.sop1(4, 20, kExec);
        a.wqm();
        a.interp(2, 0, 0);
        a.interp(3, 0, 1);
        a.vop1(1, 8, kV + 2);
        a.sop1(3, 32, kZero);  // s_movk... s32 = 0
        const std::size_t header = a.w.size();
        a.vopc(1, kZero, 2);
        a.sop1(36, 22, kVcc);
        const std::size_t skip = a.branch(8);
        a.sop1(4, kVcc, kExec);
        a.wqm();
        a.vop2(8, 8, kHalf, 2);
        a.sop1(4, kExec, kVcc);
        a.land(skip);
        a.sop1(4, kExec, 22);
        a.sopc(3, 32, 0);  // s_cmp_ge_i32 s32, s0
        const std::size_t exit = a.branch(5);
        a.vop1(1, 8, kV + 2);
        a.sop2(0, 32, 32, kZero + 1);
        a.back(2, header);
        a.land(exit);
        a.sop1(4, kExec, 20);
        a.exp_mrt0(12);
        a.w.push_back(kEndpgm);
        through_hlsl(lift_ps(a.w), "a uniform loop", {"[loop] for (;;)", "break;"});
    }
    // A loop pixels leave at different iterations, inside a kill region,
    // with the helper-lane query the lift makes of it and the discard at the end.
    {
        Asm a;
        a.sop1(4, 20, kExec);
        a.wqm();
        a.interp(2, 0, 0);
        a.interp(3, 0, 1);
        a.vopc(4, kZero, 2);           // v_cmp_gt_f32 vcc, 0, v2: kill
        a.sop2(21, 20, 20, kVcc);      // s_andn2_b64 s[20:21], s[20:21], vcc
        const std::size_t end = a.branch(4);
        a.sop2(15, kExec, kExec, 20);  // s_and_b64 exec, exec, s[20:21]
        a.wqm();
        a.vop1(8, 4, kV + 3);          // v4 = int(v3)
        a.vop1(1, 5, kZero);
        a.vop1(1, 8, kZero);
        a.sop1(4, 26, kExec);
        a.sop1(4, 24, kExec);          // the loop mask
        const std::size_t header = a.w.size();
        a.vopc(0x84, kV + 4, 5);       // v_cmp_gt_i32 vcc, v4, v5
        a.sop1(4, 28, kExec);
        a.sop2(21, kExec, 28, kVcc);
        a.sop2(21, 24, 24, kExec);
        const std::size_t exit = a.branch(4);
        a.sop2(15, kExec, 28, 24);
        a.vop2(3, 8, kOne, 8);         // v8 += 1.0
        a.vop2(37, 5, kZero + 1, 5);   // v_add_i32 v5, 1, v5
        a.back(2, header);
        a.land(exit);
        a.sop1(4, kExec, 26);
        a.land(end);
        a.sop1(4, kExec, 20);
        a.exp_mrt0(8);
        a.w.push_back(kEndpgm);
        through_hlsl(lift_ps(a.w), "a divergent loop in a kill region",
                     {"[[vk::ext_builtin_input(23 /* HelperInvocation */)]]", "QuadReadAcross", "discard;"});
    }
}

}  // namespace

int main() {
    g_have_dxc = g_dxc.load();
    if (!g_have_dxc) std::printf("gcn_hlsl_test: DXC not found (%s): printing only\n", g_dxc.error().c_str());
    swizzles();
    cndmask();
    samples_and_loops();
    std::printf("gcn_hlsl_test: %d lifts printed, %d compiled by DXC%s\n", g_printed, g_compiled, g_have_dxc ? "" : " (no DXC)");
    if (g_failures) {
        std::fprintf(stderr, "gcn_hlsl_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gcn_hlsl_test ok\n");
    return 0;
}
