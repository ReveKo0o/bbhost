// Wave width of a translated pixel shader.
//
// GCN runs 64 lanes a wave and its programs say so: EXEC and VCC are 64-bit
// masks, a lane is named 0..63. The translator maps a wave onto one Vulkan
// subgroup: EXEC is a ballot kept as (lo, hi) words, the lane is
// gl_SubgroupInvocationID, cross-lane reads are subgroup shuffles. Its SPIR-V
// (1.5) uses subgroup operations without asking for a size, so a driver whose
// subgroups are 64 wide by default (AMD's) has to compile every stage wave64 -
// pixel shaders included, where RDNA's own choice is wave32.
//
// A 32-lane subgroup is a wave whose lanes 32..63 hold no pixel. GCN runs such
// waves itself whenever it packs eight quads or fewer into one, and the
// translation then computes what GCN computes: the ballots leave the hi words
// 0 as GCN's compares leave the bits of lanes not in EXEC, and every scalar
// operation is the same operation on the same words. (Today's 64-lane
// subgroups run such waves at every triangle's edge, and NVIDIA's 32-lane
// subgroups run nothing else.) It differs from GCN only where GCN would use a
// lane 32..63, which here does not exist:
//   * a cross-lane read naming one: v_readlane_b32 of a constant lane 32..63
//     that no spill cell (below) serves, or of a lane held in a register (and
//     a v_writelane_b32 to such a lane);
//   * lanes 32..63 running at all: EXEC with bits for them that no ballot
//     made - an inversion (s_not, orn2, nand, nor, xnor), a constant, data. GCN
//     would run those lanes, and their compares would put bits into VCC and
//     EXEC that the ballots here leave 0: a loop waiting for EXEC to empty
//     would never end. A vector instruction under such an EXEC keeps the
//     program at 64.
// As a second line, a mask with such bits tested as a whole - s_cbranch_execz,
// vccz and their nz forms, EXECZ/VCCZ or a hi word as a number, the SCC of a
// 64-bit result, a 64-bit bit count, find or bit compare - keeps it at 64 too:
// where nothing above happened these still compute what GCN computes, but they
// are where a case missed above would show.
// Everything else is the same at either width: ballots, v_readfirstlane, the
// quad and 32-lane ds_swizzle modes (neither leaves a lane's half of the
// wave), v_cndmask on a mask's bit, v_mbcnt_lo/hi (no lane below 32 counts a
// hi bit), ds_append/consume (one counter step per subgroup, ranks within it)
// and EXEC save/restore.
//
// A forward dataflow over the program finds, at each instruction, the scalar
// registers whose value may be (or be the hi word of) a mask with such bits.
//
// Spill cells. Short of SGPRs, the compiler parks a scalar in one lane of a
// VGPR (v_writelane_b32 vN, sK, L) and takes it back later (v_readlane_b32
// sD, vN, L). As a subgroup shuffle the value lives in invocation L, which
// exists only when lane L holds a pixel: at a triangle's edge (a quad or two
// in the wave), in a 32-lane subgroup for L >= 32 (one pixel shader of the
// game parks s2 and s3 in lanes 32 and 33), on any device. GCN keeps every
// lane's VGPRs whether a pixel is there or not. v_writelane ignores EXEC, so
// as long as nothing else writes vN in between, the read is of exactly the
// value the write parked - a scalar, which the translator keeps in a variable
// of its own, the cell (vN, L), and reads from there. Which reads are such is
// a must-dataflow: a cell holds from its v_writelane until any other write to
// vN, met over every path into the read.
#pragma once

#include "gcn/isa.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>

namespace gcn {

// The constant-lane v_readlane_b32s of a program that read a spill cell, by
// instruction offset, and the cells (VGPR, lane) they read. Empty for a
// program with PC transfers or a fork (not followed).
struct SpillCells {
    std::set<std::uint32_t> reads;
    std::set<std::pair<int, int>> cells;
};
SpillCells spill_cells(const Program& program);
// BBHOST_SPILL_CELLS=0: no cells; every v_readlane_b32 is a subgroup shuffle.
bool spill_cells_on();

// Why a program keeps the 64-lane subgroup GCN wrote it for
// (TranslateResult::wave64_needs); 0 = it computes the same in a 32-lane one.
enum Wave64Need : std::uint32_t {
    kWave64LaneHigh = 1u << 0,     // v_readlane_b32 of a constant lane 32..63 that no spill cell serves
    kWave64LaneDynamic = 1u << 1,  // v_readlane/v_writelane_b32 of a lane held in a register
    kWave64ExecHigh = 1u << 2,     // a vector instruction under an EXEC that may have bits for lanes 32..63 no ballot made
    kWave64ExecTest = 1u << 3,     // s_cbranch_execz/execnz, EXECZ or EXEC_HI read as a number while EXEC may have them
    kWave64VccTest = 1u << 4,      // the same for VCC (s_cbranch_vccz/vccnz, VCCZ, VCC_HI)
    kWave64SccTest = 1u << 5,      // SCC of a 64-bit result that may have them, read before it is written again
    kWave64MaskCount = 1u << 6,    // s_bcnt/s_ff/s_flbit/s_bitcmp of a 64-bit mask that may have them (bcnt0/ff0/flbit_i64 always)
    kWave64Fork = 1u << 7,         // s_cbranch_g_fork/i_fork/join: a mask stack
    kWave64Indirect = 1u << 8,     // s_setpc/s_swappc/s_rfe_b64: code this pass does not see
    kWave64NotPixel = 1u << 9,     // not analysed: only pixel shaders are
};
constexpr int kWave64NeedBits = 10;

// The kWave64* reasons a pixel shader's program gives (never kWave64NotPixel).
// `cell_reads`: SpillCells::reads of the translation (empty without cells).
std::uint32_t pixel_wave64_needs(const Program& program, const std::set<std::uint32_t>& cell_reads);

// "lane >= 32, exec test", or "none".
std::string wave64_needs_str(std::uint32_t needs);
// One name per bit (bit index 0..kWave64NeedBits-1), for counters.
const char* wave64_need_name(int bit);

}  // namespace gcn
