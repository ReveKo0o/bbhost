// Translates every shader of one bundle from the dump to SPIR-V and
// validates the result with SPIRV-Tools. Skips when the dump is absent.
// `gcn_translate_test BUNDLE --sweep`: validate every shader the translator
// accepts, pixel shaders also with the image dimensions per-shader compiles
// predict, and list each invalid module (a driver compiler may crash on one).
#include "test_app0.h"
#include "gcn/container.h"
#include "gcn/isa.h"
#include "gcn/translate.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : test_app0_file("dvdroot_ps4/shader/gxgui.shaderbnd.dcx");
    std::vector<std::uint8_t> raw;
    if (!gcn::read_file(path, raw)) {
        std::printf("gcn_translate_test skipped: %s not readable\n", path.c_str());
        return kTestSkip;
    }
    std::string err;
    const std::vector<std::uint8_t> b = gcn::dcx_decompress(raw, &err);
    std::vector<gcn::BundleEntry> entries;
    if (b.empty() || !gcn::bnd4_entries(b, entries, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string msg;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        if (msg.empty()) msg = message;
    });
    spvtools::ValidatorOptions vo;
    vo.SetAllowOffsetTextureOperand(true);
    const bool sweep = argc > 2 && std::strcmp(argv[2], "--sweep") == 0;
    std::size_t n = 0, images = 0, linked = 0, inputs = 0, untranslated = 0, invalid = 0, integer_images = 0;
    std::map<std::string, std::size_t> why;  // --sweep: the first error of each shader not translated, digits folded
    for (const gcn::BundleEntry& e : entries) {
        gcn::ShaderCode code;
        if (!gcn::shader_code(e.data, code)) {
            std::fprintf(stderr, "%s: no code\n", e.name.c_str());
            return 1;
        }
        std::size_t shdr = 0;
        for (std::size_t i = 0; i + 4 <= e.data.size(); ++i) {
            if (std::memcmp(e.data.data() + i, "Shdr", 4) == 0) { shdr = i; break; }
        }
        std::uint32_t regs[16];
        std::memcpy(regs, e.data.data() + shdr + 16, 64);
        gcn::TranslateOptions o;
        o.stage = code.type == 2 ? gcn::Stage::Pixel : code.type == 4 ? gcn::Stage::Compute : gcn::Stage::Vertex;
        o.rsrc1 = regs[4];
        o.rsrc2 = regs[5];
        if (o.stage == gcn::Stage::Pixel) o.ps_input_ena = regs[8];
        if (o.stage == gcn::Stage::Vertex) o.vs_out_cntl = regs[8];
        const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
        const gcn::TranslateResult r = gcn::translate(p, o);
        if (!r.ok()) {
            if (sweep) {
                ++untranslated;
                std::string k = r.errors[0];
                if (k.find(": ") != std::string::npos) k = k.substr(k.find(": ") + 2);
                for (char& c : k) {
                    if (c >= '0' && c <= '9') c = 'N';
                }
                ++why[k];
                continue;
            }
            std::fprintf(stderr, "%s: %s\n", e.name.c_str(), r.errors[0].c_str());
            return 1;
        }
        msg.clear();
        if (!tools.Validate(r.spirv.data(), r.spirv.size(), vo)) {
            std::fprintf(stderr, "%s: invalid SPIR-V: %s\n", e.name.c_str(), msg.c_str());
            if (!sweep) return 1;
            ++invalid;
            continue;
        }
        if (sweep) {
            if (o.stage == gcn::Stage::Pixel && !r.images.empty()) {
                gcn::TranslateOptions po = o;
                for (const gcn::PredictedImage& d : gcn::predict_image_dims(p, r)) po.image_dims.emplace_back(d.dim, d.arrayed);
                const gcn::TranslateResult pr = gcn::translate(p, po);
                msg.clear();
                if (!pr.ok() || !tools.Validate(pr.spirv.data(), pr.spirv.size(), vo)) {
                    std::fprintf(stderr, "%s: with predicted image dims: %s\n", e.name.c_str(), pr.ok() ? msg.c_str() : pr.errors[0].c_str());
                    ++invalid;
                }
            }
            ++n;
            continue;
        }
        // Every T# in this bundle is traceable to user data.
        for (const gcn::ImageBinding& img : r.images) {
            if (img.path.user_sgpr < 0) {
                std::fprintf(stderr, "%s: untraceable image\n", e.name.c_str());
                return 1;
            }
        }
        images += r.images.size();
        ++n;
        // Integer texels (TranslateOptions::image_dims bits 8-9): the same
        // shader with every image read as unsigned integers must still be a
        // valid module - loads, samples, gathers and stores in the uint type.
        if (!r.images.empty()) {
            gcn::TranslateOptions io = o;
            for (const gcn::ImageBinding& img : r.images) {
                io.image_dims.emplace_back((img.cube ? 3u : img.dim) | (1u << 8), img.cube ? false : img.arrayed);
            }
            const gcn::TranslateResult ir = gcn::translate(p, io);
            msg.clear();
            if (!ir.ok() || !tools.Validate(ir.spirv.data(), ir.spirv.size(), vo)) {
                std::fprintf(stderr, "%s: with integer images: %s\n", e.name.c_str(), ir.ok() ? msg.c_str() : ir.errors[0].c_str());
                return 1;
            }
            for (const gcn::ImageBinding& img : ir.images) {
                if (img.kind != (img.depth ? 0u : 1u)) {
                    std::fprintf(stderr, "%s: image kind %u, want %u\n", e.name.c_str(), img.kind, img.depth ? 0u : 1u);
                    return 1;
                }
            }
            ++integer_images;
        }
        // Linked outputs: a vertex shader writes each param at the locations its
        // pixel shader's inputs name (here location 0 <- param 1, location 2 <-
        // param 0, location 1 unlinked), and nowhere else.
        if (o.stage == gcn::Stage::Vertex) {
            gcn::TranslateOptions lo = o;
            lo.link_outputs = true;
            lo.output_links = {1, gcn::TranslateOptions::kNoLink, 0};
            const gcn::TranslateResult lr = gcn::translate(p, lo);
            const auto has = [](const std::vector<std::uint32_t>& v, std::uint32_t x) {
                for (std::uint32_t y : v) {
                    if (y == x) return true;
                }
                return false;
            };
            std::vector<std::uint32_t> want;
            if (has(r.vs_params, 1)) want.push_back(0);
            if (has(r.vs_params, 0)) want.push_back(2);
            msg.clear();
            if (!lr.ok() || lr.vs_params != want || !tools.Validate(lr.spirv.data(), lr.spirv.size(), vo)) {
                std::fprintf(stderr, "%s: linked outputs wrong (%zu locations, %s)\n", e.name.c_str(), lr.vs_params.size(), msg.c_str());
                return 1;
            }
            ++linked;
        }
        // A pixel shader reads inputs among those its binary's semantic table
        // declares (Gnm structure at Shdr + 0x10, count at + 0x38).
        if (o.stage == gcn::Stage::Pixel) {
            const std::uint32_t declared = e.data[shdr + 16 + 0x38];
            for (std::uint32_t k : r.ps_inputs) {
                if (k >= declared) {
                    std::fprintf(stderr, "%s: reads input %u, the binary declares %u\n", e.name.c_str(), k, declared);
                    return 1;
                }
            }
            inputs += r.ps_inputs.size();
        }
    }
    if (sweep) {
        std::printf("gcn_translate_test sweep: %zu shaders translated, %zu not translated, %zu invalid modules\n", n, untranslated, invalid);
        for (const auto& [k, c] : why) std::printf("  %4zu  %s\n", c, k.c_str());
        return invalid ? 1 : 0;
    }
    std::printf("gcn_translate_test ok: %zu shaders, %zu image bindings, %zu linked vertex shaders, %zu pixel-shader inputs within their tables, "
                "%zu shaders valid with integer images\n", n, images, linked, inputs, integer_images);
    return n ? 0 : 1;
}
