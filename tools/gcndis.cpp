// gcndis: Sea Islands disassembler over From's shader bundles.
//
//   gcndis --check <bundle.dcx>...      decode every shader, report unknown opcodes
//   gcndis --list <bundle.dcx>          list entries
//   gcndis <bundle.dcx> <name-substr>   disassemble matching entries
//   gcndis --raw <file>                 disassemble a raw code blob or .vpo container
//   gcndis --wave [-v] <bundle.dcx>...  which pixel shaders may run in a 32-lane subgroup
//                                       (gcn/wave.h; -v names each one kept at 64)
#include "gcn/container.h"
#include "gcn/isa.h"
#include "gcn/wave.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

void disassemble(const gcn::ShaderCode& code) {
    const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
    for (const gcn::Inst& in : p.insts) {
        std::printf("  %06x  %08x", in.offset, in.words[0]);
        if (in.size > 1) {
            std::printf(" %08x", in.words[1]);
        } else {
            std::printf("         ");
        }
        if (in.size > 2) {
            std::printf(" %08x", in.words[2]);
        } else {
            std::printf("         ");
        }
        std::printf("  %s\n", gcn::format(in).c_str());
    }
    for (const gcn::DecodeError& e : p.errors) {
        std::printf("  ! %06x %s\n", e.offset, e.what.c_str());
    }
}

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gcndis --check <bundle.dcx>... | --wave [-v] <bundle.dcx>... | --list <bundle> | <bundle> <name> | --raw <file>\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "--raw") {
        std::vector<std::uint8_t> raw;
        if (argc < 3 || !gcn::read_file(argv[2], raw)) {
            return 1;
        }
        gcn::ShaderCode code;
        if (!gcn::shader_code(raw, code)) {
            code.words.resize(raw.size() / 4);
            std::memcpy(code.words.data(), raw.data(), code.words.size() * 4);
        }
        disassemble(code);
        return 0;
    }
    if (mode == "--wave") {
        // The renderer's wave32 census of the pixel shaders in the bundles
        // (gcn/wave.h): distinct programs by
        // their code, and why those that keep 64 lanes do.
        bool verbose = false;
        std::size_t pixel = 0, wave32 = 0, entries_seen = 0, spill_programs = 0;
        std::size_t by_reason[gcn::kWave64NeedBits] = {};
        std::map<std::vector<std::uint32_t>, std::uint32_t> seen;
        std::map<int, std::string> first;  // reason bit -> the first program kept at 64 for it
        for (int a = 2; a < argc; ++a) {
            if (std::strcmp(argv[a], "-v") == 0) {
                verbose = true;
                continue;
            }
            std::vector<gcn::BundleEntry> entries;
            if (!load_bundle(argv[a], entries)) {
                return 1;
            }
            for (const gcn::BundleEntry& e : entries) {
                gcn::ShaderCode code;
                if (!gcn::shader_code(e.data, code) || code.type != 2) {  // ShaderFileHeader type 2: a pixel shader
                    continue;
                }
                ++entries_seen;
                if (seen.count(code.words)) {
                    continue;
                }
                const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
                // The translator's spill cells, as it reads them (BBHOST_SPILL_CELLS).
                const gcn::SpillCells cells = gcn::spill_cells_on() ? gcn::spill_cells(p) : gcn::SpillCells{};
                const std::uint32_t needs = gcn::pixel_wave64_needs(p, cells.reads);
                spill_programs += !cells.reads.empty();
                if (verbose) {
                    // Each program's readlanes and how many of them read a cell.
                    std::size_t readlanes = 0;
                    for (const gcn::Inst& in : p.insts) {
                        readlanes += (in.enc == gcn::Enc::VOP2 && in.op == 1) || (in.enc == gcn::Enc::VOP3 && in.op == 0x101);
                    }
                    if (readlanes) {
                        std::printf("spills: %s: %zu of %zu v_readlane_b32 read a cell (%zu cells)\n", e.name.c_str(), cells.reads.size(),
                                    readlanes, cells.cells.size());
                    }
                }
                seen[code.words] = needs;
                ++pixel;
                if (!needs) {
                    ++wave32;
                    continue;
                }
                for (int b = 0; b < gcn::kWave64NeedBits; ++b) {
                    if (!(needs & (1u << b))) {
                        continue;
                    }
                    ++by_reason[b];
                    first.emplace(b, e.name);
                }
                if (verbose) {
                    std::printf("keeps 64: %s (%s)\n", e.name.c_str(), gcn::wave64_needs_str(needs).c_str());
                }
            }
        }
        std::printf("pixel shaders=%zu (entries %zu) wave32=%zu keep-64=%zu; reading spill cells %zu\n", pixel, entries_seen, wave32,
                    pixel - wave32, spill_programs);
        for (int b = 0; b < gcn::kWave64NeedBits; ++b) {
            if (by_reason[b]) {
                std::printf("  %-24s x%zu (first: %s)\n", gcn::wave64_need_name(b), by_reason[b], first[b].c_str());
            }
        }
        return 0;
    }
    if (mode == "--check") {
        std::size_t shaders = 0, bad = 0, insts = 0, no_end = 0;
        std::map<std::string, std::size_t> unknown;
        std::map<std::string, std::size_t> mnemonics;
        bool mnemonic_list = false;
        for (int a = 2; a < argc; ++a) {
            if (std::strcmp(argv[a], "--mnemonics") == 0) {
                mnemonic_list = true;
                continue;
            }
            std::vector<gcn::BundleEntry> entries;
            if (!load_bundle(argv[a], entries)) {
                return 1;
            }
            for (const gcn::BundleEntry& e : entries) {
                gcn::ShaderCode code;
                if (!gcn::shader_code(e.data, code)) {
                    continue;
                }
                ++shaders;
                const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
                insts += p.insts.size();
                const bool ends = !p.insts.empty() && p.insts.back().enc == gcn::Enc::SOPP && p.insts.back().op == 1;
                if (!ends) {
                    if (no_end++ == 0) {
                        std::printf("first not ending in s_endpgm: %s\n", e.name.c_str());
                    }
                }
                if (!p.errors.empty()) {
                    ++bad;
                    for (const gcn::DecodeError& err : p.errors) {
                        if (unknown[err.what]++ == 0) {
                            std::printf("first unknown: %s at %06x in %s\n", err.what.c_str(), err.offset, e.name.c_str());
                        }
                    }
                }
                for (const gcn::Inst& in : p.insts) {
                    if (const char* m = gcn::mnemonic(in)) {
                        ++mnemonics[m];
                    }
                }
            }
        }
        std::printf("shaders=%zu instructions=%zu shaders-with-unknown=%zu not-ending-in-s_endpgm=%zu distinct-mnemonics=%zu\n",
                    shaders, insts, bad, no_end, mnemonics.size());
        for (const auto& [what, n] : unknown) {
            std::printf("  unknown %-40s x%zu\n", what.c_str(), n);
        }
        if (mnemonic_list) {
            for (const auto& [m, n] : mnemonics) {
                std::printf("  %9zu %s\n", n, m.c_str());
            }
        }
        return bad ? 1 : 0;
    }
    std::vector<gcn::BundleEntry> entries;
    if (!load_bundle(mode, entries)) {
        return 1;
    }
    if (argc < 3 || std::strcmp(argv[2], "--list") == 0) {
        for (const gcn::BundleEntry& e : entries) {
            gcn::ShaderCode code;
            if (gcn::shader_code(e.data, code)) {
                std::uint32_t h0, h1, crc;
                // ShaderBinaryInfo: +8 flags|length, +12 usage-slot info, +16/+20 shaderHash0/1, +24 crc32.
                std::memcpy(&h0, e.data.data() + code.footer + 16, 4);
                std::memcpy(&h1, e.data.data() + code.footer + 20, 4);
                std::memcpy(&crc, e.data.data() + code.footer + 24, 4);
                std::printf("%8zu %6u type=%u code=%6zuB hash=%08x%08x crc=%08x %s\n", e.data.size(), e.id, code.type,
                            code.words.size() * 4, h1, h0, crc, e.name.c_str());
            } else {
                std::printf("%8zu %6u (no code) %s\n", e.data.size(), e.id, e.name.c_str());
            }
        }
        return 0;
    }
    const std::string needle = argv[2];
    for (const gcn::BundleEntry& e : entries) {
        if (e.name.find(needle) == std::string::npos) {
            continue;
        }
        gcn::ShaderCode code;
        if (!gcn::shader_code(e.data, code)) {
            std::printf("%s: no Shdr/OrbShdr\n", e.name.c_str());
            continue;
        }
        std::printf("%s (%zu dwords):\n", e.name.c_str(), code.words.size());
        disassemble(code);
    }
    return 0;
}
