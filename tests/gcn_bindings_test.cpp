// Every storage buffer the translator binds must hold the V# (or table
// pointer) the program's registers hold at each load it serves; otherwise the
// draw reads other memory than the program would. This checks the
// translator's decisions with an analysis of its own: for each pixel shader of
// the game's bundles, translated as the renderer's no-fallback variant, a
// must-dataflow over the program's control-flow graph tracks what each SGPR
// and each spill cell (a VGPR's lane at a constant index) holds - user data,
// the words of a scalar load from a known base, and copies of those (s_mov,
// v_writelane_b32 / v_readlane_b32) - meeting at joins to what every edge
// agrees on, and at every load TranslateResult::buffer_at names, the
// register pair must hold exactly the bound path's words. Any other SGPR
// write makes the register unknown, any other VGPR write forgets that VGPR's
// cells, and registers start unknown except user data, so a binding passes
// only where it holds on every path in.
//
//   gcn_bindings_test [bundle.dcx...]   (default: every bundle under BBHOST_APP0)
#include "test_app0.h"
#include "gcn/container.h"
#include "gcn/isa.h"
#include "gcn/translate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// What a register holds, as text: "U<k>" user SGPR k; "M(<base>,<v|p>,<off>)#<d>"
// dword d of the block a scalar load read at dword offset `off` from the
// pair <base> (v: through a V#, s_buffer_load; p: a pointer, s_load). A pair
// of consecutive words is "U<k>" or "L(<base>,<v|p>,<off + d>)", the form
// path_canon gives a ResourcePath.
using State = std::map<int, std::string>;  // SGPR k (0..103) or cell 1000 + vgpr * 64 + lane -> value; absent: unknown
constexpr int kCell = 1000;

std::string pair_canon(const State& s, int reg) {
    const auto a = s.find(reg), b = s.find(reg + 1);
    if (a == s.end() || b == s.end()) return "";
    const std::string& x = a->second;
    const std::string& y = b->second;
    if (x[0] == 'U' && y[0] == 'U') {
        const int k = std::stoi(x.substr(1)), k1 = std::stoi(y.substr(1));
        return k1 == k + 1 ? x : "";
    }
    if (x[0] == 'M' && y[0] == 'M') {
        // M(<block>)#d
        const std::size_t hx = x.rfind('#'), hy = y.rfind('#');
        if (x.substr(0, hx) != y.substr(0, hy)) return "";
        const int dx = std::stoi(x.substr(hx + 1)), dy = std::stoi(y.substr(hy + 1));
        if (dy != dx + 1) return "";
        // x = "M(B,f,O)#d" -> "L(B,f,O+d)"
        const std::string inner = x.substr(2, hx - 3);  // B,f,O
        const std::size_t c = inner.rfind(',');
        const int off = std::stoi(inner.substr(c + 1));
        return "L(" + inner.substr(0, c) + "," + std::to_string(off + dx) + ")";
    }
    return "";
}

std::string path_canon(const gcn::ResourcePath& p) {
    std::string cur = "U" + std::to_string(p.user_sgpr);
    if (p.immediate) return cur;
    for (const gcn::ResourceStep& st : p.loads) cur = "L(" + cur + "," + (st.vsharp ? "v" : "p") + "," + std::to_string(st.offset_dw) + ")";
    return "L(" + cur + "," + (p.final_vsharp ? "v" : "p") + "," + std::to_string(p.final_offset_dw) + ")";
}

bool const_lane(std::uint16_t code, int& lane) {
    if (code < 128 || code > 192) return false;
    lane = code - 128;
    return lane < 64;
}

void forget_cells(State& s, int vgpr) {
    for (auto it = s.lower_bound(kCell + vgpr * 64); it != s.end() && it->first < kCell + (vgpr + 1) * 64;) it = s.erase(it);
}

// The SOP1 / SOP2 operations whose result is 64 bits (a register pair).
bool sop1_wide(std::uint16_t op) {
    switch (op) {
    case 4: case 6: case 8: case 10: case 12: case 28: case 30: case 31: case 33: case 36: case 37: case 38: case 39: case 40: case 41:
    case 42: case 43: case 45: case 47: case 49:
        return true;
    default: return false;
    }
}
bool sop2_wide(std::uint16_t op) {
    switch (op) {
    case 11: case 15: case 17: case 19: case 21: case 23: case 25: case 27: case 29: case 31: case 33: case 35: case 37: case 41: case 42:
        return true;
    default: return false;
    }
}

void transfer(State& s, const gcn::Inst& in) {
    const auto unknown = [&](int reg) {
        if (reg >= 0 && reg < 104) s.erase(reg);
    };
    switch (in.enc) {
    case gcn::Enc::SMRD: {
        if (!(in.op <= 4 || (in.op >= 8 && in.op <= 12))) {  // s_memtime, s_dcache_inv: no load
            unknown(in.sdst);
            unknown(in.sdst + 1);
            return;
        }
        const int count = in.op < 8 ? (1 << (in.op & 7)) : (1 << ((in.op - 8) & 7));
        const std::string base = pair_canon(s, in.src0);
        const bool known = base.size() && (in.imm_flag || in.has_literal);
        const int off = in.imm_flag ? in.imm : static_cast<int>(in.literal);
        for (int k = 0; k < count; ++k) {
            if (known) {
                s[in.sdst + k] = "M(" + base + "," + (in.op >= 8 ? "v" : "p") + "," + std::to_string(off) + ")#" + std::to_string(k);
            } else {
                unknown(in.sdst + k);
            }
        }
        return;
    }
    case gcn::Enc::SOP1:
        if (in.op == 3) {  // s_mov_b32
            if (in.src0 < 104 && s.count(in.src0)) {
                const std::string v = s[in.src0];
                s[in.dst] = v;
            } else {
                unknown(in.dst);
            }
            return;
        }
        if (in.op == 4) {  // s_mov_b64
            std::string v0, v1;
            const bool k0 = in.src0 < 104 && s.count(in.src0), k1 = in.src0 + 1 < 104 && s.count(in.src0 + 1);
            if (k0) v0 = s[in.src0];
            if (k1) v1 = s[in.src0 + 1];
            if (k0) s[in.dst] = v0; else unknown(in.dst);
            if (k1) s[in.dst + 1] = v1; else unknown(in.dst + 1);
            return;
        }
        unknown(in.dst);
        if (sop1_wide(in.op)) unknown(in.dst + 1);
        return;
    case gcn::Enc::SOP2:
        unknown(in.dst);
        if (sop2_wide(in.op)) unknown(in.dst + 1);
        return;
    case gcn::Enc::SOPK:
        if (in.op == 0 || in.op == 2 || in.op == 15 || in.op == 16 || in.op == 18) unknown(in.dst);
        return;
    case gcn::Enc::VOP2:
    case gcn::Enc::VOP3: {
        const bool vop3 = in.enc == gcn::Enc::VOP3;
        const std::uint32_t op2 = vop3 ? (in.op >= 0x100 && in.op < 0x140 ? in.op - 0x100 : 0xffff) : in.op;
        const std::uint16_t lane_code = vop3 ? in.src1 : static_cast<std::uint16_t>(in.src1 - 256);
        if (op2 == 1) {  // v_readlane_b32 sdst, vN, lane
            int lane = 0;
            if (in.src0 >= 256 && const_lane(lane_code, lane) && s.count(kCell + (in.src0 - 256) * 64 + lane)) {
                const std::string v = s[kCell + (in.src0 - 256) * 64 + lane];
                s[in.dst] = v;
            } else {
                unknown(in.dst);
            }
            return;
        }
        if (op2 == 2) {  // v_writelane_b32 vN, s, lane
            int lane = 0;
            if (const_lane(lane_code, lane)) {
                const int cell = kCell + in.dst * 64 + lane;
                if (in.src0 < 104 && s.count(in.src0)) {
                    const std::string v = s[in.src0];
                    s[cell] = v;
                } else {
                    s.erase(cell);
                }
            } else {
                forget_cells(s, in.dst);
            }
            return;
        }
        if (vop3 && in.op == 0x182) {  // v_readfirstlane_b32 in VOP3: an SGPR
            unknown(in.dst);
            return;
        }
        if (vop3 && in.op < 0x100) {  // a compare into an SGPR pair
            unknown(in.sdst);
            unknown(in.sdst + 1);
            return;
        }
        if (vop3 && ((in.op >= 0x125 && in.op <= 0x12a) || in.op == 0x16d || in.op == 0x16e)) {
            unknown(in.sdst);
            unknown(in.sdst + 1);
        }
        forget_cells(s, in.dst);
        if (vop3 && in.op == 0x161) forget_cells(s, in.dst + 1);  // v_lshl_b64
        return;
    }
    case gcn::Enc::VOP1:
        if (in.op == 2) {  // v_readfirstlane_b32: an SGPR
            unknown(in.dst);
            return;
        }
        forget_cells(s, in.dst);
        return;
    case gcn::Enc::VINTRP:
        forget_cells(s, in.dst);
        return;
    case gcn::Enc::MIMG:
    case gcn::Enc::MUBUF:
    case gcn::Enc::MTBUF: {
        // The VGPRs a load writes from vdata: MIMG one a dmask bit (gather4
        // four), buffer loads by their op; stores none; anything else four.
        int n = 4;
        if (in.enc == gcn::Enc::MIMG) {
            if (in.op == 8 || in.op == 9) n = 0;  // image_store(_mip)
            else if (in.op >= 64 && in.op < 96) n = 4;
            else n = __builtin_popcount(in.dmask & 0xf);
        } else if (in.enc == gcn::Enc::MTBUF) {
            n = in.op < 4 ? static_cast<int>(in.op) + 1 : in.op < 8 ? 0 : 4;
        } else {
            n = in.op < 4 ? static_cast<int>(in.op) + 1 : in.op < 8 ? 0 : in.op <= 12 ? 1 : in.op == 13 ? 2 : in.op == 14 ? 4 : in.op == 15 ? 3
                : in.op >= 24 && in.op <= 30 ? 0 : 4;
        }
        if (in.tfe) ++n;
        for (int k = 0; k < n; ++k) forget_cells(s, in.vdata + k);
        return;
    }
    case gcn::Enc::DS:
        for (int k = 0; k < 4; ++k) forget_cells(s, in.dst + k);
        return;
    default:
        return;
    }
}

State meet(const State& a, const State& b) {
    State r;
    for (const auto& [k, v] : a) {
        const auto it = b.find(k);
        if (it != b.end() && it->second == v) r[k] = v;
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> bundles;
    for (int a = 1; a < argc; ++a) bundles.push_back(argv[a]);
    if (bundles.empty()) {
        for (const char* b : {"gxdecal.shaderbnd", "gxffxshader.shaderbnd", "gxflvershader.shaderbnd", "gxflvershader.shaderbnddebug",
                              "gxgui.shaderbnd", "gxposteffect.shaderbnd", "gxrenderershader.shaderbnd", "gxshader.shaderbnd"}) {
            const std::string path = test_app0_file((std::string("dvdroot_ps4/shader/") + b + ".dcx").c_str());
            if (path.empty()) break;
            bundles.push_back(path);
        }
    }
    std::size_t programs = 0, sites = 0, bad_sites = 0, bad_programs = 0, skipped = 0, read = 0;
    for (const std::string& bundle : bundles) {
        std::vector<std::uint8_t> raw;
        if (!gcn::read_file(bundle, raw)) continue;
        ++read;
        std::string err;
        const auto b = gcn::dcx_decompress(raw, &err);
        std::vector<gcn::BundleEntry> entries;
        gcn::bnd4_entries(b, entries, &err);
        for (const auto& e : entries) {
            gcn::ShaderCode code;
            if (!gcn::shader_code(e.data, code) || code.type != 2) continue;
            std::size_t shdr = 0;
            while (shdr + 0x60 <= e.data.size() && shdr < 0x100 && std::memcmp(e.data.data() + shdr, "Shdr", 4) != 0) ++shdr;
            std::uint32_t regs[16];
            std::memcpy(regs, e.data.data() + shdr + 16, sizeof(regs));
            const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
            gcn::TranslateOptions o;
            o.stage = gcn::Stage::Pixel;
            o.rsrc1 = regs[4];
            o.rsrc2 = regs[5];
            o.ps_input_ena = regs[8];
            o.descriptor_set = 1;
            o.cb_ssbo = true;
            o.cb_no_fallback = true;
            const gcn::TranslateResult r = gcn::translate(p, o);
            if (!r.ok() || r.buffer_at.empty()) continue;
            // Blocks and edges.
            bool pc = false;
            std::set<std::uint32_t> starts = {0};
            const std::uint32_t end = p.insts.empty() ? 0 : p.insts.back().offset + p.insts.back().size * 4;
            for (const auto& in : p.insts) {
                if (in.enc == gcn::Enc::SOP1 && (in.op == 32 || in.op == 33)) pc = true;
                if (in.enc == gcn::Enc::SOPP && (in.op == 2 || (in.op >= 4 && in.op <= 9))) {
                    starts.insert(in.offset + 4 + static_cast<std::uint32_t>(in.imm) * 4);
                    starts.insert(in.offset + 4);
                } else if (in.enc == gcn::Enc::SOPP && in.op == 1) {
                    starts.insert(in.offset + 4);
                }
            }
            if (pc) {
                ++skipped;
                continue;
            }
            std::map<std::uint32_t, std::vector<std::size_t>> block_insts;
            std::map<std::uint32_t, std::vector<std::uint32_t>> succ;
            {
                std::uint32_t cur = 0;
                for (std::size_t i = 0; i < p.insts.size(); ++i) {
                    const auto& in = p.insts[i];
                    if (starts.count(in.offset)) cur = in.offset;
                    block_insts[cur].push_back(i);
                }
                for (auto& [blk, idx] : block_insts) {
                    const auto& last = p.insts[idx.back()];
                    const std::uint32_t next = last.offset + last.size * 4;
                    if (last.enc == gcn::Enc::SOPP && last.op == 2) {
                        succ[blk].push_back(last.offset + 4 + static_cast<std::uint32_t>(last.imm) * 4);
                    } else if (last.enc == gcn::Enc::SOPP && last.op >= 4 && last.op <= 9) {
                        succ[blk].push_back(last.offset + 4 + static_cast<std::uint32_t>(last.imm) * 4);
                        if (next < end) succ[blk].push_back(next);
                    } else if (!(last.enc == gcn::Enc::SOPP && last.op == 1)) {
                        if (next < end) succ[blk].push_back(next);
                    }
                }
            }
            // Must-dataflow to a fixed point.
            const int nuser = static_cast<int>((o.rsrc2 >> 1) & 0x1f);
            State entry;
            for (int k = 0; k < nuser && k < 16; ++k) entry[k] = "U" + std::to_string(k);
            std::map<std::uint32_t, State> in_state;
            std::map<std::uint32_t, bool> seen;
            in_state[0] = entry;
            seen[0] = true;
            for (bool changed = true; changed;) {
                changed = false;
                for (auto& [blk, idx] : block_insts) {
                    if (!seen[blk]) continue;
                    State s = in_state[blk];
                    for (std::size_t i : idx) transfer(s, p.insts[i]);
                    for (std::uint32_t t : succ[blk]) {
                        if (!seen[t]) {
                            in_state[t] = s;
                            seen[t] = true;
                            changed = true;
                        } else {
                            State m = meet(in_state[t], s);
                            if (m != in_state[t]) {
                                in_state[t] = std::move(m);
                                changed = true;
                            }
                        }
                    }
                }
            }
            // The sites.
            ++programs;
            bool bad = false;
            for (auto& [blk, idx] : block_insts) {
                State s = in_state[blk];
                for (std::size_t i : idx) {
                    const auto& in = p.insts[i];
                    const auto site = r.buffer_at.find(in.offset);
                    if (site != r.buffer_at.end()) {
                        const int reg = in.enc == gcn::Enc::SMRD ? in.src0 : in.srsrc;
                        const std::string got = pair_canon(s, reg);
                        const std::string want = path_canon(r.buffers[site->second].path);
                        ++sites;
                        if (got != want) {
                            ++bad_sites;
                            bad = true;
                            std::printf("%s %06x: %s binds %s; the registers hold %s\n", e.name.c_str(), in.offset, gcn::mnemonic(in),
                                        want.c_str(), got.empty() ? "unknown" : got.c_str());
                        }
                    }
                    transfer(s, in);
                }
            }
            bad_programs += bad;
        }
    }
    if (!read) {
        std::printf("gcn_bindings_test skipped: no shader bundle readable (BBHOST_APP0)\n");
        return kTestSkip;
    }
    std::printf("gcn_bindings_test: %zu programs with bound loads (%zu with PC transfers skipped), %zu loads, %zu do not hold, in %zu programs\n",
                programs, skipped, sites, bad_sites, bad_programs);
    if (!programs) return 1;
    return bad_sites ? 1 : 0;
}
