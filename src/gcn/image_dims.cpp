// Image dimensions predicted from a program alone, for the shaders compiled
// when the game creates them. The renderer takes a T#'s dimensions from
// the texture a draw binds; a stage compiled when the game creates it has only
// its program. GCN programs show the type in how they sample:
//   * MIMG r128 samples a V# as a 1D image;
//   * a cube is sampled at (s, t, face), and the face is a v_cubeid_f32 result;
//   * MIMG DA marks an array sample;
//   * everything else is 2D.
// A 3D texture reads three coordinates like an array but carries no mark, so it
// is predicted 2D; the renderer's check counts such misses.
#include "gcn/isa.h"
#include "gcn/translate.h"

#include <array>
#include <cstring>

namespace gcn {

std::vector<PredictedImage> predict_image_dims(const Program& program, const TranslateResult& paths) {
    struct Votes {
        bool cube = false, plain = false, da = false, r128 = false;
    };
    std::vector<Votes> votes(paths.images.size());
    std::array<bool, 512> cube_id{};  // VGPRs holding a v_cubeid_f32 result, in address order
    const auto vgpr_write = [&](std::uint32_t dst, bool is_cube_id) {
        if (dst < cube_id.size()) cube_id[dst] = is_cube_id;
    };
    for (const Inst& in : program.insts) {
        const char* name = mnemonic(in);
        switch (in.enc) {
        case Enc::MIMG: {
            const auto at = paths.image_at.find(in.offset);
            if (at != paths.image_at.end() && at->second < votes.size()) {
                Votes& v = votes[at->second];
                v.da |= in.da;
                v.r128 |= in.r128;
                const std::uint32_t op = in.op;
                const bool sample_or_gather = op >= 32 && op < 96, sample_cd = op >= 104 && op < 112;
                if (sample_or_gather || sample_cd) {
                    // The translator's slot order (translate.cpp mimg): offset, bias,
                    // comparison reference, explicit derivatives (three pairs for a
                    // cube), then the coordinates.
                    std::uint32_t prefix = 0;
                    bool derivatives = sample_cd;
                    if (sample_or_gather) {
                        const std::uint32_t low = (op - (op < 64 ? 32 : 64));
                        prefix += (low & 16) ? 1 : 0;
                        prefix += ((low & 7) == 5 || (low & 7) == 6) ? 1 : 0;
                        prefix += (low & 8) ? 1 : 0;
                        derivatives = (low & 7) == 2 || (low & 7) == 3;
                    } else {
                        const std::uint32_t low = op - 104;
                        prefix += (low & 4) ? 1 : 0;
                        prefix += (low & 2) ? 1 : 0;
                    }
                    const std::uint32_t face = in.vaddr + prefix + (derivatives ? 6 : 0) + 2;
                    if (face < cube_id.size() && cube_id[face]) {
                        v.cube = true;
                    } else {
                        v.plain = true;
                    }
                }
            }
            for (std::uint32_t k = 0; k < 4; ++k) {
                if ((in.dmask >> k) & 1) vgpr_write(in.vdata + k, false);
            }
            break;
        }
        case Enc::VOP1: case Enc::VOP2: case Enc::VOP3: {
            if (in.enc == Enc::VOP3 && in.op < 0x100) break;  // comparisons write lane masks, not VGPRs
            const bool is_cube_id = name && std::strcmp(name, "v_cubeid_f32") == 0;
            const bool moves_cube_id = name && std::strcmp(name, "v_mov_b32") == 0 && in.src0 >= 256 && in.src0 - 256u < cube_id.size() &&
                                       cube_id[in.src0 - 256u];
            vgpr_write(in.dst, is_cube_id || moves_cube_id);
            break;
        }
        case Enc::VINTRP:
            vgpr_write(in.dst, false);
            break;
        case Enc::MTBUF:
            for (std::uint32_t k = 0; k <= (in.op & 3u); ++k) vgpr_write(in.vdata + k, false);
            break;
        default:
            break;
        }
    }
    std::vector<PredictedImage> out(votes.size());
    for (std::size_t k = 0; k < votes.size(); ++k) {
        const Votes& v = votes[k];
        PredictedImage& p = out[k];
        p.conflict = v.cube && v.plain;
        if (v.r128) {
            p.dim = 0;
        } else if (v.cube) {
            p.dim = 3;  // as the renderer reports a cube T#; the translator binds a 2D array of faces
        } else if (v.da) {
            p.arrayed = true;
        }
    }
    return out;
}

}  // namespace gcn
