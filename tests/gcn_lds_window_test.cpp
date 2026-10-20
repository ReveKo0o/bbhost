// gcn/translate.cpp: in the game's own hull draws every stage keeps the LDS
// in a buffer, a window a patch (TranslateOptions::tess_window). An access
// past the window reached memory the GPU had not mapped and lost the device
// (an RX 9070 XT and an RX 7600 XT, 2026-10-08), so each LDS access is checked
// as the GCN LDS behaves (TranslateOptions::tess_lds_bound): an offset inside
// the window - or, without one, inside the draw's region of the ring
// (StageParams::lds_bytes) - is used, a read past it gives 0 and a write is
// dropped. The LS pass, the hull shader and the domain shader each translate
// to valid SPIR-V with that check - an unsigned compare against the window's
// size where there is a window - and a stage without a window is bounded too.
#include "gcn/isa.h"
#include "gcn/translate.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

constexpr std::uint32_t kWindow = 848;  // the Forbidden Woods' hull: 3 control points, 112/96-byte strides, 14 constants

// ds_<op> in the DS encoding: op, offsets; then vdst, data1, data0, addr.
void ds(std::vector<std::uint32_t>& w, std::uint32_t op, std::uint32_t offset, std::uint32_t dst, std::uint32_t data0,
        std::uint32_t addr) {
    w.push_back(0xd8000000u | (op << 18) | (offset & 0xffff));
    w.push_back((dst << 24) | (data0 << 8) | addr);
}

// Whether the module compares something unsigned-less-than against a 32-bit
// constant equal to `bound` (bound != 0), or against anything (bound == 0).
bool has_ult_with(const std::vector<std::uint32_t>& spv, std::uint32_t bound) {
    std::vector<std::uint32_t> consts;  // ids of 32-bit constants equal to `bound`
    for (std::size_t i = 5; i < spv.size();) {
        const std::uint32_t op = spv[i] & 0xffff, len = spv[i] >> 16;
        if (!len || i + len > spv.size()) break;
        if (op == 43 && len == 4 && spv[i + 3] == bound) consts.push_back(spv[i + 2]);  // OpConstant
        if (op == 176 && len == 5) {                                                     // OpULessThan
            if (!bound) return true;
            for (const std::uint32_t c : consts) {
                if (spv[i + 4] == c) return true;
            }
        }
        i += len;
    }
    return false;
}

}  // namespace

int main() {
    // ds_read_b32 v1, v0; ds_write_b32 v0, v1 offset:4; s_endpgm
    std::vector<std::uint32_t> words;
    ds(words, 54, 0, 1, 0, 0);
    ds(words, 13, 4, 0, 1, 0);
    words.push_back(0xbf810000u);
    const gcn::Program prog = gcn::decode(words.data(), words.size());
    CHECK(prog.errors.empty() && prog.insts.size() == 3);

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string msg;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) {
        if (msg.empty()) msg = m;
    });
    struct Role {
        const char* name;
        gcn::Stage stage;
        gcn::TranslateOptions::TessRole role;
        std::uint32_t window;
    };
    const Role roles[] = {
        {"LS pass", gcn::Stage::Compute, gcn::TranslateOptions::TessRole::LsCompute, kWindow},
        {"hull shader", gcn::Stage::TessControl, gcn::TranslateOptions::TessRole::HullTcs, kWindow},
        {"domain shader", gcn::Stage::TessEval, gcn::TranslateOptions::TessRole::DomainTes, kWindow},
        {"LS pass without a window", gcn::Stage::Compute, gcn::TranslateOptions::TessRole::LsCompute, 0},
    };
    for (const Role& r : roles) {
        gcn::TranslateOptions o;
        o.stage = r.stage;
        o.tess_role = r.role;
        o.tess_window = r.window;
        o.tess_patch_control_points = 3;
        o.tess_quads = false;
        o.cs_threads[0] = 64;
        if (r.role == gcn::TranslateOptions::TessRole::HullTcs) o.descriptor_set = 0;
        const gcn::TranslateResult t = gcn::translate(prog, o);
        msg.clear();
        const bool valid = t.ok() && tools.Validate(t.spirv);
        const bool windowed = has_ult_with(t.spirv, kWindow);   // offsets checked against the window
        const bool bounded = has_ult_with(t.spirv, 0);         // some bound, the window's or the ring's
        std::printf("gcn_lds_window_test: %s: %s, %zu words%s%s\n", r.name, t.ok() ? "translated" : t.errors[0].c_str(), t.spirv.size(),
                    valid ? ", valid" : (", invalid: " + msg).c_str(),
                    windowed ? ", LDS bounded by the window" : bounded ? ", LDS bounded by the ring" : "");
        CHECK(t.ok());
        CHECK(valid);
        CHECK(windowed == (r.window != 0));
        CHECK(bounded);
    }
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("gcn_lds_window_test: ok\n");
    return 0;
}
