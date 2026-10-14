// gcn2hlsl: every lifted shader in the game's bundles printed as HLSL, compiled
// with DXC to SPIR-V for Vulkan and checked against the lift.
//
//   gcn2hlsl [--ps] [--vs] [--clip] [--O0] [--jobs N] [--only NAME[,NAME]]
//            [--out DIR] [--list FILE] [--no-dxc] [--sample N] [--oconfig PASSES] <bundle.dcx>...
//
// Each program is translated and lifted with the options the renderer uses
// when GX creates it (render.cpp precompile_ps / precompile_vs: the
// no-fallback variant, constant buffers pushed in set 2), named from its
// container's reflection, printed as HLSL (gcn/hlsl.h) and compiled by DXC
// (host/dxc.h, libdxcompiler loaded at run time). The DXC module must pass
// spirv-val and declare the lift's interface (gcn::hlsl_interface_diff).
// --clip lifts the pixel shaders with the clip-distance discard and the
// vertex shaders with the clip varying, as the draws that use them do.
//
// --out writes, per program, <name>.<stage>.lift.spv, .hlsl and .dxc.spv.
// They are made from the game's own shaders: they stay on this machine.
// --list writes one line per program: stage, name, result, compile ms, reason.
// Exit 0: every lift compiled to a valid module with the lift's interface.
#include "gcn/container.h"
#include "gcn/hlsl.h"
#include "gcn/isa.h"
#include "gcn/lift.h"
#include "gcn/names.h"
#include "gcn/translate.h"
#include "host/dxc.h"

#include <spirv-tools/libspirv.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool load_bundle(const std::string& path, std::vector<gcn::BundleEntry>& entries) {
    std::vector<std::uint8_t> raw;
    if (!gcn::read_file(path, raw)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    std::string err;
    const std::vector<std::uint8_t> b = gcn::dcx_decompress(raw, &err);
    if (b.empty() || !gcn::bnd4_entries(b, entries, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    return true;
}

std::string validate(const std::vector<std::uint32_t>& spirv) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string msg;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& pos, const char* message) {
        if (msg.empty()) msg = "word " + std::to_string(pos.index) + ": " + message;
    });
    spvtools::ValidatorOptions vo;
    vo.SetAllowOffsetTextureOperand(true);
    return tools.Validate(spirv.data(), spirv.size(), vo) ? std::string() : (msg.empty() ? "invalid" : msg);
}

bool write_bytes(const fs::path& p, const void* data, std::size_t n) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, n, f) == n;
    return std::fclose(f) == 0 && ok;
}

// A reason without its offsets and numbers, so like failures count together.
std::string reason_key(const std::string& r) {
    std::string k;
    bool digits = false;
    for (char c : r.substr(0, 120)) {
        const bool d = (c >= '0' && c <= '9');
        if (d) {
            if (!digits) k += '#';
            digits = true;
            continue;
        }
        digits = false;
        k += c;
    }
    return k;
}

struct Job {
    std::string bundle;  // the bundle's file name without its extensions
    std::string name;    // the program's name in its bundle
    bool vertex = false;
    gcn::Program program;
    gcn::TranslateOptions options;
    std::vector<std::uint8_t> container;
};

struct Totals {
    std::mutex mu;
    std::size_t programs = 0, lifted = 0, emitted = 0, compiled = 0, valid = 0, same_interface = 0;
    std::map<std::string, std::size_t> not_lifted, not_emitted, not_compiled, invalid, interface_diffs;
    std::uint64_t emit_us = 0, compile_us = 0, max_compile_us = 0;
    std::string slowest;
    std::vector<std::uint32_t> compile_ms;  // per compiled program
    std::uint64_t lift_words = 0, dxc_words = 0, hlsl_bytes = 0;
};

}  // namespace

int main(int argc, char** argv) {
    bool ps = false, vs = false, clip = false, use_dxc = true;
    int opt_level = 3;
    int sample = 1;  // every n-th program
    std::string passes;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency() / 2);
    std::set<std::string> only;
    const char* out_dir = nullptr;
    const char* list_path = nullptr;
    std::vector<std::string> bundles;
    for (int a = 1; a < argc; ++a) {
        const std::string arg = argv[a];
        if (arg == "--ps") ps = true;
        else if (arg == "--vs") vs = true;
        else if (arg == "--clip") clip = true;
        else if (arg == "--O0") opt_level = 0;
        else if (arg == "--oconfig" && a + 1 < argc) passes = argv[++a];
        else if (arg == "--sample" && a + 1 < argc) sample = std::max(1, std::atoi(argv[++a]));
        else if (arg == "--no-dxc") use_dxc = false;
        else if (arg == "--jobs" && a + 1 < argc) jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++a])));
        else if (arg == "--out" && a + 1 < argc) out_dir = argv[++a];
        else if (arg == "--list" && a + 1 < argc) list_path = argv[++a];
        else if (arg == "--only" && a + 1 < argc) {
            const std::string s = argv[++a];
            for (std::size_t at = 0; at <= s.size();) {
                const std::size_t comma = std::min(s.find(',', at), s.size());
                if (comma > at) only.insert(s.substr(at, comma - at));
                at = comma + 1;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 2;
        } else {
            bundles.push_back(arg);
        }
    }
    if (bundles.empty()) {
        std::fprintf(stderr, "usage: gcn2hlsl [--ps] [--vs] [--clip] [--O0] [--jobs N] [--only NAME,..] [--out DIR] [--list FILE] [--no-dxc] <bundle.dcx>...\n");
        return 2;
    }
    if (!ps && !vs) ps = vs = true;
    if (out_dir) fs::create_directories(out_dir);
    gpu::Dxc dxc;
    if (use_dxc && !dxc.load()) {
        std::fprintf(stderr, "DXC: %s\n", dxc.error().c_str());
        return 2;
    }
    if (use_dxc) std::printf("DXC: %s (%s)\n", dxc.version().c_str(), dxc.path().c_str());

    // The programs, with the options the renderer lifts them with at creation.
    std::vector<Job> work;
    std::size_t seen = 0;
    for (const std::string& path : bundles) {
        std::vector<gcn::BundleEntry> entries;
        if (!load_bundle(path, entries)) return 1;
        for (gcn::BundleEntry& e : entries) {
            gcn::ShaderCode code;
            if (!gcn::shader_code(e.data, code)) continue;
            std::size_t shdr = 0;
            while (shdr + 0x60 <= e.data.size() && shdr < 0x100 && std::memcmp(e.data.data() + shdr, "Shdr", 4) != 0) ++shdr;
            if (shdr + 0x60 > e.data.size() || shdr >= 0x100) continue;
            const bool is_ps = code.type == 2, is_vs = code.type == 1;
            if (!(is_ps && ps) && !(is_vs && vs)) continue;
            std::string base = e.name;
            if (const std::size_t slash = base.find_last_of("\\/"); slash != std::string::npos) base = base.substr(slash + 1);
            if (!only.empty() && !only.count(base) && !only.count(e.name)) continue;
            // The entry's folders too: one program name can be in several (Lightprepass0\, Deferred\).
            std::string full = e.name;
            for (char& c : full) {
                if (c == '\\' || c == '/') c = '~';
            }
            std::uint32_t regs[16];
            std::memcpy(regs, e.data.data() + shdr + 16, sizeof(regs));
            Job j;
            j.bundle = fs::path(path).filename().string();
            j.bundle = j.bundle.substr(0, j.bundle.find('.'));
            j.name = full;
            j.vertex = is_vs;
            j.program = gcn::decode(code.words.data(), code.words.size());
            const std::size_t gnm = shdr + 16;
            if (is_ps) {
                // render.cpp precompile_ps
                gcn::TranslateOptions paths_options;
                paths_options.stage = gcn::Stage::Pixel;
                paths_options.rsrc1 = regs[4];
                paths_options.rsrc2 = regs[5];
                const gcn::TranslateResult paths = gcn::translate(j.program, paths_options);
                gcn::TranslateOptions& o = j.options;
                o.stage = gcn::Stage::Pixel;
                o.rsrc1 = regs[4];
                o.rsrc2 = regs[5];
                o.ps_input_ena = regs[8];
                o.descriptor_set = 1;
                o.cb_descriptor_set = 2;
                o.cb_binding_base = gcn::kBindingStorageBuffer0 + gcn::kMaxBuffers;
                o.cb_ssbo = true;
                o.cb_no_fallback = true;
                o.exec_known = true;
                for (const gcn::PredictedImage& pi : gcn::predict_image_dims(j.program, paths)) o.image_dims.emplace_back(pi.dim, pi.arrayed);
                o.sampler_force_unnormalized.assign(paths.samplers.size(), false);
                const std::uint32_t declared_inputs = e.data[gnm + 0x38];
                const std::size_t input_table = gnm + 0x3c + static_cast<std::size_t>(e.data[gnm + 3]) * 4;
                std::uint32_t read_inputs = 0, registers[32] = {};
                bool inputs_ok = true;
                for (std::uint32_t k : paths.ps_inputs) {
                    if (k >= 32 || k >= declared_inputs || input_table + 2 * (k + 1) > e.data.size()) {
                        inputs_ok = false;
                        break;
                    }
                    std::uint16_t word;
                    std::memcpy(&word, e.data.data() + input_table + 2 * k, 2);
                    if (((word >> 12) | (word >> 10)) & 1) o.ps_flat_mask |= 1u << k;
                    const std::uint32_t semantic = word & 0xff;
                    if (semantic < 0xf || semantic - 0xf >= 32) {
                        inputs_ok = false;
                        break;
                    }
                    read_inputs |= 1u << k;
                    registers[k] = semantic - 0xf;
                }
                if (inputs_ok) {
                    for (std::uint32_t k = 0; k < 32; ++k) {
                        if (!((read_inputs >> k) & 1)) continue;
                        while (o.ps_input_map.size() < k) o.ps_input_map.push_back(static_cast<std::uint8_t>(o.ps_input_map.size()));
                        o.ps_input_map.push_back(static_cast<std::uint8_t>(registers[k] & 0x1f));
                    }
                }
                if (clip) {
                    int used = -1;
                    for (std::uint8_t l : o.ps_input_map) used = std::max(used, static_cast<int>(l));
                    for (const gcn::Inst& in : j.program.insts) {
                        if (in.enc == gcn::Enc::VINTRP && in.attr < 32) used = std::max(used, static_cast<int>(in.attr));
                    }
                    if (used + 1 <= 29) o.ps_clip_discard = 4u | static_cast<std::uint32_t>(used + 1) << 8;
                }
            } else {
                // render.cpp precompile_vs
                const std::size_t inputs = e.data[gnm + 0x24], input_table = gnm + 0x28 + static_cast<std::size_t>(e.data[gnm + 3]) * 4;
                bool ok = inputs > 0 && inputs <= 16 && input_table + inputs * 4 <= e.data.size();
                std::vector<gcn::VertexElement> elements;
                for (std::size_t k = 0; ok && k < inputs; ++k) {
                    gcn::VertexElement el;
                    el.location = static_cast<std::uint32_t>(k);
                    el.vdata = e.data[input_table + 4 * k + 1];
                    el.count = e.data[input_table + 4 * k + 2];
                    if (el.count == 0 || el.count > 4) ok = false;
                    elements.push_back(el);
                }
                if (!ok) continue;  // the renderer compiles none at creation (no usable input table)
                const std::uint32_t rsrc1 = (regs[4] & ~(3u << 24)) | (3u << 24), rsrc2 = regs[5];
                gcn::TranslateOptions paths_options;
                paths_options.stage = gcn::Stage::Vertex;
                paths_options.rsrc1 = rsrc1;
                paths_options.rsrc2 = rsrc2;
                const gcn::TranslateResult paths = gcn::translate(j.program, paths_options);
                gcn::TranslateOptions& o = j.options;
                o.stage = gcn::Stage::Vertex;
                o.rsrc1 = rsrc1;
                o.rsrc2 = rsrc2;
                o.vs_out_cntl = regs[8];
                if (clip) {
                    const std::uint32_t c = regs[8];
                    if ((c & (1u << 22)) && (c & 0xf) && !(c & 0xfff0) && !(c & (1u << 23))) {
                        int used = -1;
                        for (const gcn::Inst& in : j.program.insts) {
                            if (in.enc == gcn::Enc::EXP && in.tgt >= 32 && in.tgt < 64) used = std::max(used, static_cast<int>(in.tgt) - 32);
                        }
                        if (used + 1 <= 29) {
                            o.vs_out_cntl |= gcn::TranslateOptions::kVsOutCntlClipVarying |
                                             static_cast<std::uint32_t>(used + 1) << gcn::TranslateOptions::kVsOutCntlClipLocationShift;
                        }
                    }
                }
                o.vertex_input = elements;
                o.vertex_formats_from_params = true;
                o.link_outputs = true;
                for (std::uint8_t k = 0; k < 32; ++k) o.output_links.push_back(k);
                o.descriptor_set = 0;
                o.cb_descriptor_set = 2;
                for (const gcn::PredictedImage& pi : gcn::predict_image_dims(j.program, paths)) o.image_dims.emplace_back(pi.dim, pi.arrayed);
                o.sampler_force_unnormalized.assign(paths.samplers.size(), false);
                o.cb_ssbo = true;
                o.cb_no_fallback = true;
                o.exec_known = true;
            }
            j.container = std::move(e.data);
            if (seen++ % static_cast<std::size_t>(sample) == 0) work.push_back(std::move(j));
        }
    }

    Totals t;
    FILE* list = list_path ? std::fopen(list_path, "w") : nullptr;
    std::mutex list_mu;
    std::atomic<std::size_t> next{0};
    gpu::DxcOptions dopt;
    dopt.optimize = opt_level;
    dopt.passes = passes;
    const auto worker = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= work.size()) return;
            const Job& j = work[i];
            const char* stage = j.vertex ? "vs" : "ps";
            std::string result, reason;
            std::uint64_t ms = 0;
            const auto line = [&] {
                if (!list) return;
                std::lock_guard<std::mutex> lk(list_mu);
                std::fprintf(list, "%s %s/%s %s %llu %s\n", stage, j.bundle.c_str(), j.name.c_str(), result.c_str(), static_cast<unsigned long long>(ms), reason.c_str());
            };
            {
                std::lock_guard<std::mutex> lk(t.mu);
                ++t.programs;
            }
            if (!j.program.errors.empty()) continue;
            const gcn::TranslateResult ref = gcn::translate(j.program, j.options);
            if (!ref.ok()) {
                result = "untranslated", reason = ref.errors[0];
                line();
                continue;
            }
            gcn::LiftResult l = j.vertex ? gcn::lift_vertex_shader(j.program, j.options, ref) : gcn::lift_pixel_shader(j.program, j.options, ref);
            if (!l.ok()) {
                result = "not-lifted", reason = l.rejections.empty() ? "no module" : l.rejections[0];
                std::lock_guard<std::mutex> lk(t.mu);
                t.not_lifted[reason_key(reason)]++;
                line();
                continue;
            }
            {
                gcn::ShaderReflection refl;
                if (gcn::parse_reflection(j.container, refl)) gcn::name_lift(l, ref, refl);
            }
            {
                std::lock_guard<std::mutex> lk(t.mu);
                ++t.lifted;
                t.lift_words += l.spirv.size();
            }
            const fs::path stem = out_dir ? fs::path(out_dir) / j.bundle / (j.name + "." + stage) : fs::path();
            if (out_dir) fs::create_directories(stem.parent_path());
            if (out_dir) write_bytes(stem.string() + ".lift.spv", l.spirv.data(), l.spirv.size() * 4);
            const auto e0 = std::chrono::steady_clock::now();
            gcn::HlslOptions hopt;
            hopt.title = j.name + " (" + (j.vertex ? "vertex" : "pixel") + " shader)";
            const gcn::HlslResult h = gcn::emit_hlsl(l.spirv, hopt);
            const std::uint64_t emit_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - e0).count());
            if (out_dir && !h.source.empty()) write_bytes(stem.string() + ".hlsl", h.source.data(), h.source.size());
            if (!h.ok()) {
                result = "not-emitted", reason = h.errors[0];
                std::lock_guard<std::mutex> lk(t.mu);
                t.not_emitted[reason_key(reason)]++;
                t.emit_us += emit_us;
                line();
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(t.mu);
                ++t.emitted;
                t.emit_us += emit_us;
                t.hlsl_bytes += h.source.size();
            }
            if (!use_dxc) {
                result = "emitted";
                line();
                continue;
            }
            std::vector<std::uint32_t> spirv;
            std::string error;
            const auto c0 = std::chrono::steady_clock::now();
            const bool compiled = dxc.compile(h, dopt, spirv, error);
            const std::uint64_t us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c0).count());
            ms = us / 1000;
            if (!compiled) {
                result = "not-compiled", reason = error;
                std::lock_guard<std::mutex> lk(t.mu);
                t.not_compiled[reason_key(error.substr(0, error.find('\n')))]++;
                line();
                continue;
            }
            if (out_dir) write_bytes(stem.string() + ".dxc.spv", spirv.data(), spirv.size() * 4);
            {
                std::lock_guard<std::mutex> lk(t.mu);
                ++t.compiled;
                t.compile_us += us;
                t.compile_ms.push_back(static_cast<std::uint32_t>(ms));
                t.dxc_words += spirv.size();
                if (us > t.max_compile_us) {
                    t.max_compile_us = us;
                    t.slowest = j.name + "." + stage;
                }
            }
            const std::string v = validate(spirv);
            if (!v.empty()) {
                result = "invalid", reason = v;
                std::lock_guard<std::mutex> lk(t.mu);
                t.invalid[reason_key(v)]++;
                line();
                continue;
            }
            std::string diff = gcn::hlsl_interface_diff(l.spirv, spirv);
            if (diff.empty()) {
                const std::string ops = gcn::hlsl_operation_diff(l.spirv, spirv);
                if (!ops.empty()) diff = "operations: " + ops;
            }
            std::lock_guard<std::mutex> lk(t.mu);
            ++t.valid;
            if (diff.empty()) {
                ++t.same_interface;
                result = "ok";
            } else {
                t.interface_diffs[reason_key(diff)]++;
                result = diff.rfind("operations", 0) == 0 ? "operations" : "interface", reason = diff;
            }
            line();
        }
    };
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < jobs; ++k) threads.emplace_back(worker);
    for (std::thread& th : threads) th.join();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (list) std::fclose(list);

    std::printf("programs=%zu lifted=%zu emitted=%zu compiled=%zu valid=%zu same_interface=%zu (%.1f s wall, %u jobs)\n", t.programs, t.lifted,
                t.emitted, t.compiled, t.valid, t.same_interface, wall, jobs);
    std::printf("words: lifted %llu, dxc %llu; HLSL %.1f MiB; emit %.2f s total\n", static_cast<unsigned long long>(t.lift_words),
                static_cast<unsigned long long>(t.dxc_words), t.hlsl_bytes / 1048576.0, t.emit_us / 1e6);
    if (!t.compile_ms.empty()) {
        std::vector<std::uint32_t> ms = t.compile_ms;
        std::sort(ms.begin(), ms.end());
        std::printf("DXC %s: %.1f s summed over %zu programs; per program median %u ms, p90 %u ms, p99 %u ms, max %.0f ms (%s)\n",
                    (passes.empty() ? "-O" + std::to_string(opt_level) : "-Oconfig=" + passes).c_str(),
                    t.compile_us / 1e6, ms.size(), ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms[ms.size() * 99 / 100], t.max_compile_us / 1e3,
                    t.slowest.c_str());
    }
    const auto dump = [](const char* what, const std::map<std::string, std::size_t>& m) {
        std::vector<std::pair<std::size_t, std::string>> v;
        for (const auto& [k, n] : m) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (std::size_t i = 0; i < v.size() && i < 30; ++i) std::printf("  %s x%zu: %s\n", what, v[i].first, v[i].second.c_str());
    };
    dump("not lifted", t.not_lifted);
    dump("not emitted", t.not_emitted);
    dump("not compiled", t.not_compiled);
    dump("invalid", t.invalid);
    dump("interface or operations", t.interface_diffs);
    return t.same_interface == t.lifted ? 0 : 1;
}
