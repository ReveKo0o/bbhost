// Wave width of a translated pixel shader: see wave.h.
#include "gcn/wave.h"

#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <vector>

namespace gcn {
namespace {

// Per instruction: which scalar registers (by operand code) may hold bits for
// lanes 32..63 that no ballot made - for a 64-bit pair it is the pair's hi
// register that counts - and whether SCC was last set from a 64-bit result
// that may.
struct MaskState {
    std::bitset<128> x;
    bool scc = false;
    bool reached = false;
    bool join(const MaskState& o) {
        const std::bitset<128> nx = x | o.x;
        const bool ns = scc || o.scc;
        const bool changed = !reached || nx != x || ns != scc;
        x = nx;
        scc = ns;
        reached = true;
        return changed;
    }
};

// The lane a v_readlane/v_writelane_b32 names, if it is a constant (the
// translator takes it modulo 64); -1 for a register.
int constant_lane(std::uint16_t code, const Inst& in) {
    if (code >= 128 && code <= 192) return (code - 128) & 63;
    if (code >= 193 && code <= 208) return (-static_cast<int>(code - 192)) & 63;
    if (code == kLiteral) return static_cast<int>(in.literal & 63);
    return -1;
}
// The lane of a spill cell: an inline constant 0..63, the lanes the
// translator's own const_lane takes (anything else is never a cell).
int cell_lane(std::uint16_t code) { return code >= 128 && code < 192 ? code - 128 : -1; }

bool vector_enc(Enc e) {
    switch (e) {
    case Enc::VOP2: case Enc::VOP1: case Enc::VOPC: case Enc::VOP3: case Enc::VINTRP:
    case Enc::DS: case Enc::MUBUF: case Enc::MTBUF: case Enc::MIMG: case Enc::EXP:
        return true;
    default:
        return false;
    }
}

struct MaskFlow {
    const std::set<std::uint32_t>* cell_reads = nullptr;  // SpillCells::reads
    std::uint32_t* needs = nullptr;                       // set on the last pass, which reports
    void need(std::uint32_t bit) {
        if (needs) *needs |= bit;
    }

    // The hi word of a 64-bit operand.
    static bool hi(const MaskState& s, std::uint16_t code) {
        if (code < 127) return s.x[code + 1];
        if (code >= 128 && code <= 192) return false;        // 0..64
        if (code == kLiteral || code == kScc) return false;  // zero-extended, as the translator reads them
        return true;                                         // negative integers, floats, anything else
    }
    // A 32-bit scalar source as a hi word.
    static bool word(const MaskState& s, std::uint16_t code, const Inst& in) {
        if (code == 128) return false;
        if (code == kLiteral) return in.literal != 0;
        if (code < 128) return s.x[code];
        return true;
    }
    static void set(MaskState& s, std::uint16_t code, bool v) {
        if (code < 128) s.x[code] = v;
    }
    static void set_pair(MaskState& s, std::uint16_t code, bool v) {
        if (code < 127) {
            s.x[code + 1] = v;
            if (code & 1) s.x[code] = true;  // misaligned: both words are suspect
        }
    }
    // 32-bit reads of a hi word, EXECZ or VCCZ: the mask as a number.
    void check_source(const MaskState& s, std::uint16_t code) {
        if ((code == kExecHi || code == kExecz) && s.x[kExecHi]) need(kWave64ExecTest);
        if ((code == kVccHi || code == kVccz) && s.x[kVccHi]) need(kWave64VccTest);
    }
    void read_scc(const MaskState& s) {
        if (s.scc) need(kWave64SccTest);
    }

    void step(const Inst& in, MaskState& s) {
        // GCN would run lanes 32..63 here; at wave32 they do not exist.
        if (vector_enc(in.enc) && s.x[kExecHi]) need(kWave64ExecHigh);
        switch (in.enc) {
        case Enc::SOP2: sop2(in, s); break;
        case Enc::SOPK: sopk(in, s); break;
        case Enc::SOP1: sop1(in, s); break;
        case Enc::SOPC:
            check_source(s, in.src0);
            check_source(s, in.src1);
            if ((in.op == 14 || in.op == 15) && hi(s, in.src0)) need(kWave64MaskCount);  // s_bitcmp0/1_b64
            if (in.op <= 15) s.scc = false;                                              // from 32-bit values or one bit
            break;
        case Enc::SOPP:
            if (in.op == 4 || in.op == 5) read_scc(s);                               // s_cbranch_scc0/scc1
            if ((in.op == 6 || in.op == 7) && s.x[kVccHi]) need(kWave64VccTest);    // s_cbranch_vccz/vccnz
            if ((in.op == 8 || in.op == 9) && s.x[kExecHi]) need(kWave64ExecTest);  // s_cbranch_execz/execnz
            break;
        case Enc::SMRD: {
            const int count = in.op <= 4 ? 1 << in.op : (in.op >= 8 && in.op <= 12) ? 1 << (in.op - 8) : in.op == 30 ? 2 : 0;
            for (int k = 0; k < count; ++k) set(s, static_cast<std::uint16_t>(in.sdst + k), true);  // data
            break;
        }
        case Enc::VOP2:
            check_source(s, in.src0);
            vop2(in.op, in, s, false);
            break;
        case Enc::VOP1:
            check_source(s, in.src0);
            if (in.op == 2) set(s, in.dst, true);  // v_readfirstlane_b32: data
            break;
        case Enc::VOPC:
            check_source(s, in.src0);
            s.x[kVccHi] = false;                          // a ballot
            if (in.op & 0x10) s.x[kExecHi] = false;       // v_cmpx: EXEC too
            break;
        case Enc::VOP3:
            check_source(s, in.src0);
            check_source(s, in.src1);
            check_source(s, in.src2);
            if (in.op < 0x100) {  // a compare: its pair is a ballot
                set_pair(s, in.sdst, false);
                if (in.op & 0x10) s.x[kExecHi] = false;
            } else if (in.op < 0x140) {
                vop2(in.op - 0x100, in, s, true);
            } else if (in.op == 0x16d || in.op == 0x16e) {  // v_div_scale_f32/f64: a ballot like VCC's
                set_pair(s, in.sdst, false);
            } else if (in.op == 0x182) {  // v_readfirstlane_b32
                set(s, in.dst, true);
            }
            break;
        default:
            break;  // the rest write VGPRs, memory or exports only
        }
    }

    void vop2(std::uint32_t op, const Inst& in, MaskState& s, bool vop3) {
        // VOP2 keeps a VGPR's code in src1; readlane/writelane read it as a scalar code.
        const std::uint16_t lane_code = vop3 ? in.src1 : static_cast<std::uint16_t>(in.src1 - 256);
        switch (op) {
        case 1: {  // v_readlane_b32: an SGPR from another lane
            const int lane = constant_lane(lane_code, in);
            if (lane < 0) need(kWave64LaneDynamic);
            else if (lane >= 32 && !cell_reads->count(in.offset)) need(kWave64LaneHigh);
            set(s, in.dst, true);
            break;
        }
        case 2:  // v_writelane_b32: of a constant lane, harmless itself (what reads the lane is judged)
            if (constant_lane(lane_code, in) < 0) need(kWave64LaneDynamic);
            break;
        case 37: case 38: case 39: case 40: case 41: case 42:  // carry out: a ballot
            if (vop3) {
                set_pair(s, in.sdst, false);
            } else {
                s.x[kVccHi] = false;
            }
            break;
        default:
            break;
        }
    }

    void sop2(const Inst& in, MaskState& s) {
        check_source(s, in.src0);
        check_source(s, in.src1);
        const std::uint32_t op = in.op;
        if (op == 11 || op == 15 || op == 17 || op == 19 || op == 21 || op == 23 || op == 25 || op == 27 || op == 29 || op == 31 ||
            op == 33 || op == 35 || op == 37 || op == 41 || op == 42) {  // 64-bit results
            const bool a = hi(s, in.src0), b = hi(s, in.src1);
            bool r = true;
            switch (op) {
            case 11: read_scc(s); r = a || b; break;  // s_cselect_b64
            case 15: r = a && b; break;               // s_and_b64: a ballot's hi word clears the other's
            case 17: case 19: r = a || b; break;      // s_or_b64, s_xor_b64
            case 21: r = a; break;                    // s_andn2_b64: a & ~b
            default: r = true; break;                 // inversions (orn2, nand, nor, xnor), shifts, bit fields
            }
            set_pair(s, in.dst, r);
            if (op != 11 && op != 37) s.scc = r;  // SCC = result != 0; s_cselect_b64 and s_bfm_b64 leave it
            return;
        }
        if (op == 43) {  // s_cbranch_g_fork
            need(kWave64Fork);
            return;
        }
        if (op == 4 || op == 5 || op == 10) read_scc(s);  // s_addc_u32, s_subb_u32, s_cselect_b32
        set(s, in.dst, true);
        if (op <= 9 || (op >= 14 && op <= 28 && !(op & 1)) || op == 30 || op == 32 || op == 34 || op == 39 || op == 40 || op == 44) {
            s.scc = false;  // from a 32-bit result or carry
        }
    }

    void sopk(const Inst& in, MaskState& s) {
        switch (in.op) {
        case 0: set(s, in.dst, in.imm != 0); break;  // s_movk_i32
        case 2:                                      // s_cmovk_i32
            read_scc(s);
            if (in.imm != 0) set(s, in.dst, true);
            break;
        case 15: set(s, in.dst, true); s.scc = false; break;  // s_addk_i32
        case 16: case 18: set(s, in.dst, true); break;        // s_mulk_i32, s_getreg_b32
        case 17: need(kWave64Fork); break;                    // s_cbranch_i_fork
        default:
            if (in.op >= 3 && in.op <= 14) {  // s_cmpk_*: the register as a number
                check_source(s, in.dst);
                s.scc = false;
            }
            break;
        }
    }

    void sop1(const Inst& in, MaskState& s) {
        check_source(s, in.src0);
        const std::uint16_t d = in.dst;
        const bool a = hi(s, in.src0);
        switch (in.op) {
        case 3: set(s, d, word(s, in.src0, in)); break;  // s_mov_b32
        case 4: set_pair(s, d, a); break;                // s_mov_b64
        case 5:                                          // s_cmov_b32
            read_scc(s);
            if (word(s, in.src0, in)) set(s, d, true);
            break;
        case 6:  // s_cmov_b64
            read_scc(s);
            if (a) set_pair(s, d, true);
            break;
        case 7: set(s, d, true); s.scc = false; break;           // s_not_b32
        case 8: set_pair(s, d, true); s.scc = true; break;       // s_not_b64: bits for every absent lane
        case 9: set(s, d, true); s.scc = false; break;           // s_wqm_b32
        case 10: set_pair(s, d, a); s.scc = a; break;            // s_wqm_b64: a quad never straddles the words
        case 12: case 30: case 31: set_pair(s, d, true); break;  // s_brev_b64, s_bitset1_b64, s_getpc_b64
        case 33: need(kWave64Indirect); set_pair(s, d, true); break;  // s_swappc_b64
        case 13: case 15: case 44: case 52: set(s, d, true); s.scc = false; break;  // s_bcnt*_b32, s_quadmask_b32, s_abs_i32
        case 14: case 18: case 24:  // s_bcnt0_i32_b64, s_ff0_i32_b64, s_flbit_i32_i64: the absent lanes count
            need(kWave64MaskCount);
            set(s, d, true);
            if (in.op == 14) s.scc = false;
            break;
        case 16: case 20: case 22:  // s_bcnt1_i32_b64, s_ff1_i32_b64, s_flbit_i32_b64
            if (a) need(kWave64MaskCount);
            set(s, d, true);
            if (in.op == 16) s.scc = false;
            break;
        case 27: case 28: break;  // s_bitset0_b32/b64: clears a bit
        case 32: case 34: need(kWave64Indirect); break;  // s_setpc_b64, s_rfe_b64
        case 36: case 37: case 38: case 39: case 40: case 41: case 42: case 43: {  // s_*_saveexec_b64
            const bool e = s.x[kExecHi];
            bool ne = true;  // orn2, nand, nor, xnor
            if (in.op == 36) ne = a && e;
            else if (in.op == 37 || in.op == 38) ne = a || e;
            else if (in.op == 39) ne = a;  // s0 & ~exec
            set_pair(s, d, e);
            s.x[kExecHi] = ne;
            s.scc = ne;
            break;
        }
        case 45: set_pair(s, d, true); s.scc = true; break;  // s_quadmask_b64
        case 47: set_pair(s, d, true); break;               // s_movrels_b64
        case 48: case 49: s.x.set(); break;                 // s_movreld_b32/b64: any register
        case 50: need(kWave64Fork); break;                  // s_cbranch_join
        default: set(s, d, true); break;                    // the other 32-bit results
        }
    }
};

// The program's control flow, by instruction index: a branch's target (one
// outside the program ends the path, as the translator's dispatcher does) and
// the next instruction unless the program ends or jumps away there.
// `transfers`: it has s_setpc/s_rfe, a fork or a join, whose successors are
// not known here.
std::vector<std::vector<std::size_t>> successors(const std::vector<Inst>& insts, bool& transfers) {
    const std::size_t n = insts.size();
    std::unordered_map<std::uint32_t, std::size_t> at;
    for (std::size_t i = 0; i < n; ++i) at[insts[i].offset] = i;
    std::vector<std::vector<std::size_t>> succ(n);
    transfers = false;
    for (std::size_t i = 0; i < n; ++i) {
        const Inst& in = insts[i];
        bool next = i + 1 < n;
        if (in.enc == Enc::SOPP && (in.op == 1 || in.op == 2 || (in.op >= 4 && in.op <= 9))) {
            if (in.op != 1) {
                const std::uint32_t target = in.offset + 4 + static_cast<std::uint32_t>(in.imm) * 4;
                if (auto it = at.find(target); it != at.end()) succ[i].push_back(it->second);
            }
            if (in.op == 1 || in.op == 2) next = false;  // s_endpgm, s_branch
        } else if (in.enc == Enc::SOP1 && (in.op == 32 || in.op == 34 || in.op == 50)) {
            next = false;  // s_setpc, s_rfe, s_cbranch_join
            transfers = true;
        } else if ((in.enc == Enc::SOP2 && in.op == 43) || (in.enc == Enc::SOPK && in.op == 17)) {
            transfers = true;  // s_cbranch_g_fork, s_cbranch_i_fork
        }
        if (next) succ[i].push_back(i + 1);
    }
    return succ;
}

// What an instruction does to the spill cells: the VGPRs it writes other than
// through v_writelane_b32 (first, count), or every VGPR. Generous where
// unsure: a cell given up is only read by shuffle again, as before cells.
struct VgprWrite {
    int first = 0, count = 0;
    bool all = false;
};
VgprWrite vgpr_write(const Inst& in) {
    VgprWrite w;
    const bool scalar = in.enc == Enc::SOP1 || in.enc == Enc::SOP2 || in.enc == Enc::SOPK || in.enc == Enc::SOPC ||
                        in.enc == Enc::SOPP || in.enc == Enc::SMRD;
    const char* name = mnemonic(in);
    if (!name && !scalar) {
        w.all = true;  // an opcode the decoder does not name
        return w;
    }
    const std::string mn = name ? name : "";
    const bool wide = mn.find("64") != std::string::npos || mn.find("sad") != std::string::npos;  // f64/b64/u64 results, (m)qsad
    switch (in.enc) {
    case Enc::SOP1:
        if (in.op == 33) w.all = true;  // s_swappc_b64: the fetch shader writes the vertex's VGPRs
        return w;
    case Enc::VOP1:
    case Enc::VOP2:
    case Enc::VOP3: {
        const std::uint32_t op = in.enc == Enc::VOP1 ? in.op + 0x180u : in.enc == Enc::VOP2 ? in.op + 0x100u : in.op;
        if (op < 0x100 || op == 0x101 || op == 0x182) return w;  // compares, v_readlane_b32, v_readfirstlane_b32: SGPRs
        if (op == 0x102) return w;                               // v_writelane_b32: the cells' own write
        if (mn.find("movreld") != std::string::npos || mn.find("movrelsd") != std::string::npos) {
            w.all = true;  // a VGPR chosen by M0
            return w;
        }
        w.first = in.dst;
        w.count = wide ? 4 : 1;
        return w;
    }
    case Enc::VINTRP:
        w.first = in.dst;
        w.count = 1;
        return w;
    case Enc::DS: {
        if (mn.rfind("ds_write", 0) == 0) return w;  // stores (the exchanges that return are ds_wrxchg*)
        const auto has = [&](const char* t) { return mn.find(t) != std::string::npos; };
        // ds_read(2)(st64)_b32/b64/b96/b128, the returning atomics and
        // exchanges (ds_wrxchg2_rtn_*: two values), ds_swizzle, append.
        const bool two = has("read2") || has("xchg2");
        w.first = in.dst;
        w.count = has("b128") || (two && has("64")) ? 4 : has("b96") ? 3 : has("64") || two ? 2 : 1;
        return w;
    }
    case Enc::MUBUF:
    case Enc::MTBUF: {
        if (mn.find("store") != std::string::npos || mn.find("wbinv") != std::string::npos) return w;
        const bool atomic = mn.find("atomic") != std::string::npos;
        if (atomic && !in.glc) return w;  // no value returned
        const auto has = [&](const char* t) { return mn.find(t) != std::string::npos; };
        // buffer_load_dword(x2|x3|x4), *_format_x(y)(z)(w); an atomic returns
        // its data's width (cmpswap's is its pair).
        w.first = in.vdata;
        w.count = atomic ? (has("x2") ? 4 : 2) : has("xyzw") || has("x4") ? 4 : has("xyz") || has("x3") ? 3 : has("xy") || has("x2") ? 2 : 1;
        w.count += in.tfe ? 1 : 0;
        return w;
    }
    case Enc::MIMG: {
        if (mn.find("store") != std::string::npos) return w;
        const bool atomic = mn.find("atomic") != std::string::npos;
        if (atomic && !in.glc) return w;
        // A component per dmask bit (a gather's four whatever it selects), and
        // the TFE/LWE status after them.
        int n = 0;
        for (int k = 0; k < 4; ++k) n += (in.dmask >> k) & 1;
        if (mn.find("gather4") != std::string::npos || atomic) n = 4;
        w.first = in.vdata;
        w.count = std::max(n, 1) + (in.tfe || in.lwe ? 1 : 0);
        return w;
    }
    default:
        return w;  // VOPC (VCC), scalar, exports
    }
}

}  // namespace

bool spill_cells_on() {
    static const bool on = [] {
        const char* e = std::getenv("BBHOST_SPILL_CELLS");
        return !(e && e[0] == '0');
    }();
    return on;
}

SpillCells spill_cells(const Program& program) {
    SpillCells out;
    const std::vector<Inst>& insts = program.insts;
    const std::size_t n = insts.size();
    const auto is_writelane = [](const Inst& in) { return (in.enc == Enc::VOP2 && in.op == 2) || (in.enc == Enc::VOP3 && in.op == 0x102); };
    const auto is_readlane = [](const Inst& in) { return (in.enc == Enc::VOP2 && in.op == 1) || (in.enc == Enc::VOP3 && in.op == 0x101); };
    bool any = false;
    for (const Inst& in : insts) any |= is_writelane(in);
    if (!any) return out;  // nothing parked
    bool transfers = false;
    const std::vector<std::vector<std::size_t>> succ = successors(insts, transfers);
    if (transfers) return out;
    // VOP2 keeps a VGPR's code in src1; readlane/writelane read it as a scalar code.
    const auto lane_of = [](const Inst& in) {
        return cell_lane(in.enc == Enc::VOP3 ? in.src1 : static_cast<std::uint16_t>(in.src1 - 256));
    };
    // The cells that hold their lane on every path to each instruction. An
    // unreached state stands for every cell until a path reaches it.
    using Cells = std::set<std::pair<int, int>>;
    const auto forget = [](Cells& c, int vgpr) {
        for (auto it = c.lower_bound({vgpr, 0}); it != c.end() && it->first == vgpr;) it = c.erase(it);
    };
    const auto step = [&](const Inst& in, Cells& c) {
        if (is_writelane(in)) {
            const int lane = lane_of(in);
            if (lane < 0) {
                forget(c, in.dst);  // a lane not known here: any of them
            } else {
                c.insert({in.dst, lane});
            }
            return;
        }
        const VgprWrite w = vgpr_write(in);
        if (w.all) {
            c.clear();
            return;
        }
        for (int v = w.first; v < w.first + w.count; ++v) forget(c, v);
    };
    std::vector<Cells> state(n);
    std::vector<bool> reached(n, false);
    reached[0] = true;
    std::deque<std::size_t> work{0};
    std::vector<bool> queued(n, false);
    queued[0] = true;
    while (!work.empty()) {
        const std::size_t i = work.front();
        work.pop_front();
        queued[i] = false;
        Cells c = state[i];
        step(insts[i], c);
        for (std::size_t j : succ[i]) {
            bool changed = false;
            if (!reached[j]) {
                state[j] = c;
                reached[j] = true;
                changed = true;
            } else {
                for (auto it = state[j].begin(); it != state[j].end();) {
                    if (c.count(*it)) {
                        ++it;
                    } else {
                        it = state[j].erase(it);
                        changed = true;
                    }
                }
            }
            if (changed && !queued[j]) {
                queued[j] = true;
                work.push_back(j);
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Inst& in = insts[i];
        if (!is_readlane(in) || !reached[i] || in.src0 < 256) continue;
        const int lane = lane_of(in);
        if (lane >= 0 && state[i].count({in.src0 - 256, lane})) {
            out.reads.insert(in.offset);
            out.cells.insert({in.src0 - 256, lane});
        }
    }
    return out;
}

std::uint32_t pixel_wave64_needs(const Program& program, const std::set<std::uint32_t>& cell_reads) {
    const std::vector<Inst>& insts = program.insts;
    const std::size_t n = insts.size();
    if (n == 0) return 0;
    bool transfers = false;
    const std::vector<std::vector<std::size_t>> succ = successors(insts, transfers);  // each transfer is a reason of its own
    std::vector<MaskState> state(n);
    MaskState entry;
    entry.x.set();             // user data, and whatever the registers hold
    entry.x[kVccHi] = false;   // VCC starts 0 in the translation
    entry.x[kExecHi] = false;  // EXEC starts as ballot(true)
    state[0].join(entry);
    MaskFlow flow;
    flow.cell_reads = &cell_reads;
    std::deque<std::size_t> work{0};
    std::vector<bool> queued(n, false);
    queued[0] = true;
    while (!work.empty()) {
        const std::size_t i = work.front();
        work.pop_front();
        queued[i] = false;
        MaskState out = state[i];
        flow.step(insts[i], out);
        for (std::size_t j : succ[i]) {
            if (state[j].join(out) && !queued[j]) {
                queued[j] = true;
                work.push_back(j);
            }
        }
    }
    // The states are final: one more pass over what is reachable reports.
    std::uint32_t needs = 0;
    flow.needs = &needs;
    for (std::size_t i = 0; i < n; ++i) {
        if (!state[i].reached) continue;
        MaskState s = state[i];
        flow.step(insts[i], s);
    }
    return needs;
}

const char* wave64_need_name(int bit) {
    static const char* const kNames[kWave64NeedBits] = {"lane >= 32",    "lane from a register", "exec with lanes >= 32",
                                                        "exec test",     "vcc test",             "scc of a mask",
                                                        "mask count",    "fork",                 "indirect",
                                                        "not a pixel shader"};
    return bit >= 0 && bit < kWave64NeedBits ? kNames[bit] : "?";
}

std::string wave64_needs_str(std::uint32_t needs) {
    std::string s;
    for (int b = 0; b < kWave64NeedBits; ++b) {
        if (!(needs & (1u << b))) continue;
        if (!s.empty()) s += ", ";
        s += wave64_need_name(b);
    }
    return s.empty() ? "none" : s;
}

}  // namespace gcn
