// Pixel shaders at wave32 (gcn/wave.h): which programs may run in a 32-lane
// subgroup, and the spill cells that make SGPR spills exact at any width.
// Hand-encoded programs, then - when the dump is there (BBHOST_APP0) - every
// pixel shader of the game: each must run at wave32 with cells, and every
// constant-lane v_readlane_b32 must read a cell.
#include "test_app0.h"
#include "gcn/container.h"
#include "gcn/isa.h"
#include "gcn/translate.h"
#include "gcn/wave.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Sea Islands encodings of the few instructions the cases use.
std::uint32_t sopp(std::uint32_t op, std::int16_t simm) { return 0xbf800000u | op << 16 | static_cast<std::uint16_t>(simm); }
std::uint32_t sop1(std::uint32_t op, std::uint32_t sdst, std::uint32_t ssrc) { return 0xbe800000u | sdst << 16 | op << 8 | ssrc; }
std::uint32_t vop2(std::uint32_t op, std::uint32_t vdst, std::uint32_t vsrc1, std::uint32_t src0) { return op << 25 | vdst << 17 | vsrc1 << 9 | src0; }
std::uint32_t vop1(std::uint32_t op, std::uint32_t vdst, std::uint32_t src0) { return 0x7e000000u | vdst << 17 | op << 9 | src0; }
std::uint32_t vopc(std::uint32_t op, std::uint32_t vsrc1, std::uint32_t src0) { return 0x7c000000u | op << 17 | vsrc1 << 9 | src0; }
constexpr std::uint32_t kEndpgm = 0xbf810000u;
constexpr std::uint32_t kExec = 126, kM1 = 193;                     // EXEC, the inline constant -1
std::uint32_t writelane(std::uint32_t v, std::uint32_t s, std::uint32_t lane) { return vop2(2, v, 128 + lane, s); }
std::uint32_t readlane(std::uint32_t s, std::uint32_t v, std::uint32_t lane) { return vop2(1, s, 128 + lane, 256 + v); }

gcn::Program program(const std::vector<std::uint32_t>& words) { return gcn::decode(words.data(), words.size()); }
std::uint32_t needs(const gcn::Program& p, bool cells) {
    return gcn::pixel_wave64_needs(p, cells ? gcn::spill_cells(p).reads : std::set<std::uint32_t>{});
}

void synthetic() {
    // A spill to lane 33 and back: a cell at 32 lanes; without cells, lane 33 keeps 64.
    const gcn::Program spill = program({writelane(3, 4, 33), readlane(5, 3, 33), kEndpgm});
    expect(gcn::spill_cells(spill).reads.size() == 1, "a spill and its reload read a cell");
    expect(needs(spill, true) == 0, "a spill to lane 33 runs at wave32 with cells");
    expect(needs(spill, false) == gcn::kWave64LaneHigh, "a spill to lane 33 keeps 64 without cells");
    // Another write to the VGPR in between: the lane is no longer the spill's.
    const gcn::Program clobbered = program({writelane(3, 4, 33), vop1(1, 3, 128), readlane(5, 3, 33), kEndpgm});
    expect(gcn::spill_cells(clobbered).reads.empty(), "a VGPR written in between has no cell");
    expect(needs(clobbered, true) == gcn::kWave64LaneHigh, "and its lane 33 keeps 64");
    // A write to another VGPR, and a sample into v26 with one component, leave v3's cell.
    const std::uint32_t sample0 = 0xf0800100u, sample1 = 1u << 16 | 26u << 8;  // image_sample v26, v0, s[4:11], s[0:3] dmask:0x1
    const gcn::Program kept = program({writelane(30, 4, 2), vop1(1, 29, 128), sample0, sample1, readlane(5, 30, 2), kEndpgm});
    expect(gcn::spill_cells(kept).reads.size() == 1, "writes beside the spill VGPR keep its cell");
    // A spill on one path only: no cell where the paths meet.
    const gcn::Program one_path = program({sopp(4, 1), writelane(3, 4, 33), readlane(5, 3, 33), kEndpgm});
    expect(gcn::spill_cells(one_path).reads.empty(), "a cell holds only on every path");
    expect(needs(one_path, true) == gcn::kWave64LaneHigh, "and the read keeps 64");
    // Lanes below 32 are the same at either width, cells or not.
    expect(needs(program({writelane(3, 4, 7), readlane(5, 3, 7), kEndpgm}), false) == 0, "lane 7 runs at wave32");
    // A lane from a register.
    expect(needs(program({vop2(1, 5, 6, 256 + 3), kEndpgm}), true) == gcn::kWave64LaneDynamic, "v_readlane of an SGPR's lane keeps 64");
    // EXEC inverted, then lanes run: GCN would run lanes 32..63.
    const gcn::Program inverted = program({sop1(8, kExec, kExec), vop1(1, 1, 128), kEndpgm});
    expect((needs(inverted, true) & gcn::kWave64ExecHigh) != 0, "a vector instruction under ~EXEC keeps 64");
    // EXEC = -1, a loop until a compare clears it: the ballots' hi words would never clear.
    const gcn::Program loop = program({sop1(4, kExec, kM1), vopc(0x14, 256 + 1, 256 + 0), sopp(9, -2), kEndpgm});
    expect((needs(loop, true) & (gcn::kWave64ExecHigh | gcn::kWave64ExecTest)) != 0, "a loop on an EXEC of -1 keeps 64");
    // The usual: whole-quad mode, a v_cmpx kill and its s_cbranch_execz.
    const gcn::Program kill = program({sop1(4, 88, kExec), sop1(10, kExec, kExec), vopc(0x14, 256 + 1, 256 + 0), sopp(8, 1), vop1(1, 2, 128),
                                       sop1(4, kExec, 88), kEndpgm});
    expect(needs(kill, true) == 0, "whole-quad mode, v_cmpx and s_cbranch_execz run at wave32");
    // The translator reads the cell: no shuffle left in the module.
    gcn::TranslateOptions o;
    o.stage = gcn::Stage::Pixel;
    const gcn::TranslateResult r = gcn::translate(spill, o);
    expect(r.wave64_needs == (gcn::spill_cells_on() ? 0u : gcn::kWave64LaneHigh), "the translation carries the reasons");
    if (gcn::spill_cells_on()) {
        bool shuffle = false;
        for (std::size_t i = 5; i < r.spirv.size();) {
            const std::uint32_t n = r.spirv[i] >> 16;
            shuffle |= (r.spirv[i] & 0xffff) == 345;  // OpGroupNonUniformShuffle
            i += n ? n : 1;
        }
        expect(!shuffle, "a cell read is no shuffle");
    }
    gcn::TranslateOptions vs;
    vs.stage = gcn::Stage::Vertex;
    expect(gcn::translate(program({kEndpgm}), vs).wave64_needs == gcn::kWave64NotPixel, "other stages are not analysed");
}

}  // namespace

int main(int argc, char** argv) {
    synthetic();
    // The game's pixel shaders, when the dump is there.
    std::size_t programs = 0, readlanes = 0, cell_reads = 0;
    const char* bundles[] = {"gxdecal", "gxffxshader", "gxflvershader", "gxgui", "gxposteffect", "gxrenderershader", "gxshader"};
    std::set<std::vector<std::uint32_t>> seen;
    for (const char* name : bundles) {
        const std::string path = argc > 1 ? std::string(argv[1]) + "/" + name + ".shaderbnd.dcx"
                                          : test_app0_file((std::string("dvdroot_ps4/shader/") + name + ".shaderbnd.dcx").c_str());
        std::vector<std::uint8_t> raw;
        if (!gcn::read_file(path, raw)) continue;
        std::string err;
        const std::vector<std::uint8_t> b = gcn::dcx_decompress(raw, &err);
        std::vector<gcn::BundleEntry> entries;
        if (b.empty() || !gcn::bnd4_entries(b, entries, &err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        for (const gcn::BundleEntry& e : entries) {
            gcn::ShaderCode code;
            if (!gcn::shader_code(e.data, code) || code.type != 2 || !seen.insert(code.words).second) continue;
            const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
            const gcn::SpillCells cells = gcn::spill_cells(p);
            const std::uint32_t n = gcn::pixel_wave64_needs(p, cells.reads);
            if (n) std::fprintf(stderr, "FAIL: %s keeps 64 (%s)\n", e.name.c_str(), gcn::wave64_needs_str(n).c_str());
            failures += n != 0;
            for (const gcn::Inst& in : p.insts) readlanes += (in.enc == gcn::Enc::VOP2 && in.op == 1) || (in.enc == gcn::Enc::VOP3 && in.op == 0x101);
            cell_reads += cells.reads.size();
            ++programs;
        }
    }
    if (programs) {
        expect(cell_reads == readlanes, "every v_readlane_b32 of the game reads a cell");
        std::printf("gcn_wave_test: %zu pixel programs of the game at wave32, %zu of %zu readlanes from cells\n", programs, cell_reads, readlanes);
    }
    if (failures) return 1;
    std::printf("gcn_wave_test ok\n");
    return 0;
}
