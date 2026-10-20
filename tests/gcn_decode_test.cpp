// Decodes every shader in one bundle from the game dump with the Sea Islands
// decoder: no unknown opcodes, every program ends in s_endpgm. Skips when the
// dump is absent.
#include "test_app0.h"
#include "gcn/container.h"
#include "gcn/isa.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : test_app0_file("dvdroot_ps4/shader/gxgui.shaderbnd.dcx");
    std::vector<std::uint8_t> raw;
    if (!gcn::read_file(path, raw)) {
        std::printf("gcn_decode_test skipped: %s not readable\n", path.c_str());
        return kTestSkip;
    }
    std::string err;
    const std::vector<std::uint8_t> b = gcn::dcx_decompress(raw, &err);
    std::vector<gcn::BundleEntry> entries;
    if (b.empty() || !gcn::bnd4_entries(b, entries, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::size_t shaders = 0, insts = 0;
    for (const gcn::BundleEntry& e : entries) {
        gcn::ShaderCode code;
        if (!gcn::shader_code(e.data, code)) {
            std::fprintf(stderr, "%s: no code\n", e.name.c_str());
            return 1;
        }
        const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
        if (!p.errors.empty()) {
            std::fprintf(stderr, "%s: %s at %06x\n", e.name.c_str(), p.errors[0].what.c_str(), p.errors[0].offset);
            return 1;
        }
        if (p.insts.empty() || p.insts.back().enc != gcn::Enc::SOPP || p.insts.back().op != 1) {
            std::fprintf(stderr, "%s: does not end in s_endpgm\n", e.name.c_str());
            return 1;
        }
        // Every instruction formats without an unknown marker.
        for (const gcn::Inst& in : p.insts) {
            if (gcn::format(in).find("<unknown") != std::string::npos) {
                std::fprintf(stderr, "%s: bad format at %06x\n", e.name.c_str(), in.offset);
                return 1;
            }
        }
        ++shaders;
        insts += p.insts.size();
    }
    std::printf("gcn_decode_test ok: %zu shaders, %zu instructions\n", shaders, insts);
    return shaders ? 0 : 1;
}
