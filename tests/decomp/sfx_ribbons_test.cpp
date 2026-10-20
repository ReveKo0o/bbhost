// The effect ribbons' writers (decomp/sfx/ribbons.cpp) against the game's own
// code (tests/decomp/eboot_kit.h): sub_2cce7b0 and sub_2cceec0 run in the loaded
// eboot on the same random strips as ours, every byte of the vertices
// compared, under the console's floating-point mode. Skips without the 1.09
// eboot (BBHOST_EBOOT, or eboot-109-decrypted.bin in the checkout).
#include "decomp/sfx/ribbons.cpp"

#include "eboot_kit.h"

#include <cstdio>


namespace {

eboot_kit::Rng g_rng(0x5f3759df);

bool chance(double p) { return g_rng.chance(p); }
float value(float lo, float hi) { return g_rng.value(lo, hi); }

struct Strip {
    static constexpr int kPoints = 16;  // the writers read up to point n + 1
    float born[kPoints], px[kPoints], py[kPoints], pz[kPoints], u_offset[kPoints], nx[kPoints], ny[kPoints], nz[kPoints];
    u32 packed[kPoints];
    u32 n, first, total, per_point;
    bool offsets;
    float f[16];
};

Strip random_strip() {
    Strip s{};
    s.n = chance(0.9) ? static_cast<u32>(g_rng.range(1, 3)) : static_cast<u32>(g_rng.range(4, 7));
    const float now = value(0, 100);
    const float base = std::isfinite(now) ? now : 50.0f;
    for (int i = 0; i < Strip::kPoints; ++i) {
        s.born[i] = value(base - 6, base + 1);
        s.px[i] = value(-300, 300);
        s.py[i] = value(-300, 300);
        s.pz[i] = value(-300, 300);
        s.u_offset[i] = value(-1, 1);
        s.nx[i] = value(-1, 1);
        s.ny[i] = value(-1, 1);
        s.nz[i] = value(-1, 1);
        s.packed[i] = g_rng.u32();
    }
    if (chance(0.8)) {
        s.first = 4 * static_cast<u32>(g_rng.range(0, 40));
        s.total = s.first + s.n;
    } else {
        s.first = g_rng.u32();
        s.total = g_rng.u32();
    }
    s.per_point = chance(0.45) ? 1 : chance(0.9) ? 0 : 2;
    s.offsets = chance(0.7);
    s.f[0] = now;
    for (int i = 1; i < 16; ++i) s.f[i] = value(-2, 6);
    return s;
}

// Floats where both sides made a NaN, of whichever payload - an operand
// order the compiler was free to swap - count as the same.
bool same_vertices(const std::uint8_t* a, const std::uint8_t* b, std::size_t len, std::size_t* where) {
    for (std::size_t k = 0; k < len; k += 4) {
        u32 x, y;
        std::memcpy(&x, a + k, 4);
        std::memcpy(&y, b + k, 4);
        if (x == y) continue;
        const std::size_t field = k % kVertex;
        const bool is_float = field <= 0x08 || field == 0x20;
        if (is_float && eboot_kit::same_float(std::bit_cast<float>(x), std::bit_cast<float>(y))) continue;
        *where = k;
        return false;
    }
    return true;
}

int run(const char* name, void* game_fn, bool along, int cases) {
    int bad = 0;
    for (int c = 0; c < cases; ++c) {
        const Strip s = random_strip();
        const std::size_t len = static_cast<std::size_t>(s.n) * 2 * kVertex;
        std::uint8_t theirs[16 * kVertex], ours[16 * kVertex];
        std::memset(theirs, 0xa5, sizeof theirs);
        std::memset(ours, 0xa5, sizeof ours);
        const float* off = s.offsets ? s.u_offset : nullptr;
        const float* f = s.f;
        if (!along) {
            reinterpret_cast<FacingFn>(game_fn)(theirs, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                                                s.packed, off, f[8], f[9], f[10], f[11], f[12], s.first, s.total, f[13], f[14]);
            sfx_ribbon_facing(ours, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], s.packed, off, f[8],
                              f[9], f[10], f[11], f[12], s.first, s.total, f[13], f[14]);
        } else {
            reinterpret_cast<AlongFn>(game_fn)(theirs, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                                               s.packed, off, s.nx, s.ny, s.nz, s.per_point, f[8], f[9], s.first, s.total, f[10],
                                               f[11]);
            sfx_ribbon_along(ours, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], s.packed, off, s.nx,
                             s.ny, s.nz, s.per_point, f[8], f[9], s.first, s.total, f[10], f[11]);
        }
        std::size_t k = 0;
        // Past the strip neither may have written.
        if (!same_vertices(theirs, ours, len, &k) || std::memcmp(theirs + len, ours + len, sizeof theirs - len) != 0) {
            if (++bad <= 8) {
                u32 a = 0, b = 0;
                std::memcpy(&a, theirs + k, 4);
                std::memcpy(&b, ours + k, 4);
                std::printf("%s: case %d (n %u, first %u, total %u%s) differs at vertex %zu +0x%zx: the game's 0x%08x, ours 0x%08x\n",
                            name, c, s.n, s.first, s.total, g_rng.specials ? ", special values" : "", k / kVertex, k % kVertex, a, b);
            }
        }
    }
    return bad;
}

}  // namespace

int main() {
    eboot_kit::load("decomp_sfx_ribbons");
    std::uint8_t* facing = eboot_kit::at(0x2cce7b0);
    std::uint8_t* along = eboot_kit::at(0x2cceec0);
    if (std::memcmp(facing, kFacingEntry, sizeof kFacingEntry) != 0 || std::memcmp(along, kAlongEntry, sizeof kAlongEntry) != 0) {
        std::printf("decomp_sfx_ribbons: the writers' entries are not where the decomp expects them\n");
        return 1;
    }
    int bad = 0;
    constexpr int kCases = 200000;
    for (const bool specials : {false, true}) {
        g_rng.specials = specials;
        bad += run("sub_2cce7b0", facing, false, kCases);
        bad += run("sub_2cceec0", along, true, kCases);
    }
    std::printf("decomp_sfx_ribbons: %d strips each way (ordinary values, then special ones), %d differ\n", 2 * kCases, bad);
    return bad ? 1 : 0;
}
