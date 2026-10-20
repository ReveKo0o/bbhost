// The last points of an effect ribbon's vertex strip (docs/decomp.md):
// sub_2cce7b0 and sub_2cceec0 at Binary Ninja 0x2cce7b0 and 0x2cceec0.
//
// A ribbon effect turns its points into a strip of two vertices a point,
// 0x38 bytes each, in memory the GPU reads: sub_2cf83e0, one effect type's
// draw, through sub_2ec4270, and sub_2eb4510 likewise. Four points at a time
// go through sub_2ce7a90 or sub_2ce88f0, and the last one to three through
// these two, the last point first.
//
// The game's versions keep their pointer arguments below rsp: the red zone,
// 128 bytes that the SysV ABI promises no signal or interrupt will touch.
// Windows has no red zone. An exception there builds its frame from rsp
// down, over those bytes, and the texture write watch takes an exception for
// the first write to a page it watches - which the strips' pages often are.
// When that write was one of these functions', its pointers came back zeroed
// and the next point read through a null one: ACCESS_VIOLATION at guest
// 0x2cce9b5, the crash the community's "Intel 12th Gen+ SFX workaround"
// (emoose's patch, a return at sub_2cf83e0, which draws no ribbon of that
// type at all) is for. On Linux the kernel skips the red zone when it
// delivers a signal, and nothing goes wrong.
//
// Ours computes the same bits and keeps nothing below rsp (the Windows build
// passes -mno-red-zone, CMakeLists.txt), so a fault in its writes costs only
// the fault. Of the 162,959 functions the eboot's .eh_frame lists, 325 touch
// their red zone, and these two write where the watch faults: the strips'
// 4-wide writers (which keep nothing below rsp) are among the busiest
// writers in its census, and these finish the same strips.
#include "decomp/decomp.h"
#include "decomp/guest.h"

#include "core/write_watch.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include <xmmintrin.h>

#include "log.h"

using namespace decomp;

// Bit for bit: every operation in the game's order, and none fused into
// another - the file is built with -ffp-contract=off (CMakeLists.txt).

namespace {

constexpr std::size_t kVertex = 0x38;

// Reads of the game's arrays at a byte offset, as its code indexes them.
inline u32 at_u32(const void* base, u32 offset) {
    u32 v;
    std::memcpy(&v, static_cast<const std::uint8_t*>(base) + offset, sizeof v);
    return v;
}

// vcvtsi2ss from a register holding a zero-extended 32-bit value.
inline float from_u32(u32 v) { return static_cast<float>(static_cast<std::int64_t>(v)); }
// vcvttss2si into a 64-bit register, of which the game keeps the low half
// (out of range, the instruction's 0x8000000000000000: a low half of 0).
inline u32 trunc32(float f) { return static_cast<u32>(_mm_cvttss_si64(_mm_set_ss(f))); }
inline float sqrt_ss(float f) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(f))); }
// The sign flipped, as vxorps with the sign mask does it.
inline float negate(float f) { return std::bit_cast<float>(std::bit_cast<u32>(f) ^ 0x80000000u); }
// 1 / length, or 0 where that is infinite (a length of 0); a NaN stays.
inline float inverse(float length) {
    const float r = 1.0f / length;
    return (std::bit_cast<u32>(r) & 0x7fffffffu) == 0x7f800000u ? 0.0f : r;
}
inline float length2(float x, float y, float z) { return z * z + (x * x + y * y); }

// A unit vector packed into a dword, (axis * scale + 127) a byte: x in the
// low byte, y shifted by 8, z by 16 with the bits above it kept.
inline u32 pack_unit(float x, float y, float z, float scale) {
    const u32 bx = trunc32(x * scale + 127.0f) & 0xffu;
    const u32 by = trunc32((y * scale + 127.0f) * 256.0f) & 0xff00u;
    const u32 bz = trunc32((z * scale + 127.0f) * 65536.0f) & 0xffff0000u;
    return bz | (by | bx);
}

// A colour channel from [0, 1]: all ones above 1, else (c * 255 * shift)
// truncated and masked; none at all below 0.
inline u32 channel(float c, float shift, u32 mask) {
    const u32 v = c > 1.0f ? mask : trunc32(c * 255.0f * shift) & mask;
    return 0.0f > c ? 0 : v;
}
inline u32 blue_channel(float c) {
    const u32 v = c > 1.0f ? 0xffu : trunc32(c * 255.0f) & 0xffu;
    return 0.0f > c ? 0 : v;
}

// The fade at point index t: in over the first `in` points, out over the
// last `out` (inv_in, inv_out their reciprocals, 1 where they are not
// positive), 1 between.
inline float fade(float t, float rest, float in, float inv_in, float out, float inv_out) {
    const float end = out > rest ? inv_out * rest : 1.0f;
    return in > t ? inv_in * t : end;
}

// The opacity left at age/life - 1: 1 at birth, 0 from the end of life on,
// falling as the square between.
inline float age_falloff(float x) {
    float a = 0.0f;
    if (!(x > -0.0f)) a = x < -1.0f ? 1.0f : negate(x);
    return a * a;
}

inline u32 alpha_channel(float a) {
    if (0.0f > a) return 0;
    return a > 1.0f ? 0xff000000u : trunc32(a * 255.0f * 16777216.0f) & 0xff000000u;
}

inline void put(std::uint8_t* at, u32 v) { std::memcpy(at, &v, sizeof v); }
inline void put_f(std::uint8_t* at, float f) { std::memcpy(at, &f, sizeof f); }

// One vertex: position, three packed unit vectors, colour, the point's
// packed value, u, v and 16 zero bytes.
inline void put_vertex(std::uint8_t* q, float x, float y, float z, u32 n0, u32 n1, u32 n2, u32 colour, u32 packed, float u,
                       u32 v_bits) {
    put_f(q + 0x00, x);
    put_f(q + 0x04, y);
    put_f(q + 0x08, z);
    put(q + 0x0c, n0);
    put(q + 0x10, n1);
    put(q + 0x14, n2);
    put(q + 0x18, colour);
    put(q + 0x1c, packed);
    put_f(q + 0x20, u);
    put(q + 0x24, v_bits);
    std::memset(q + 0x28, 0, 0x10);
}

// The game's u-offset table when a ribbon has none: 16 zero bytes at
// 0x4c9ab80 (sub_2cce7b0) and 0x4c9ab90 (sub_2cceec0).
constexpr u32 kNoOffsets[4] = {};

std::atomic<std::uint64_t> g_facing{0}, g_along{0};

}  // namespace

// sub_2cce7b0: a strip that faces the eye. Each point's pair straddles it
// along cross(eye - point, next point - point), half_width to each side;
// the vertices carry the way to the eye, that cross and the way to the next
// point, packed.
DECOMP_LEAF void sfx_ribbon_facing(std::uint8_t* out, u32 n, const float* born, const float* px, const float* py,
                                   const float* pz, float now, float eye_x, float eye_y, float eye_z, float life, float v,
                                   float half_width, float u0, const u32* packed, const float* u_offset, float u_span,
                                   float red, float green, float blue, float opacity, u32 first, u32 total, float fade_in,
                                   float fade_out) {
    g_facing.fetch_add(1, std::memory_order_relaxed);
    const float inv_life = 0.0f >= life ? -1.0f : 1.0f / life;
    const float inv_in = fade_in > 0.0f ? 1.0f / fade_in : 1.0f;
    const float inv_out = fade_out > 0.0f ? 1.0f / fade_out : 1.0f;
    const u32 v_bits = std::bit_cast<u32>(v);
    const float u_end = u0 + u_span;
    const float last = from_u32(total - 1);
    float t = from_u32(first + n);
    const void* offsets = u_offset ? static_cast<const void*>(u_offset) : kNoOffsets;
    const u32 offset_step = u_offset ? 4 : 0;
    const u32 r = channel(red, 65536.0f, 0xff0000u);
    const u32 g = channel(green, 256.0f, 0xff00u);
    const u32 b = blue_channel(blue);
    u32 i4 = n * 4 - 4;                 // the point, as a byte offset into the arrays
    u32 o4 = (n - 1) * offset_step;     // its u offset
    std::uint8_t* pair = out + static_cast<std::uint64_t>(n + n) * kVertex - 2 * kVertex;
    do {
        const u32 point_packed = at_u32(packed, i4);
        const float offset = std::bit_cast<float>(at_u32(offsets, o4));
        const float age = now - load<float>(born, i4);
        const float x = load<float>(px, i4), y = load<float>(py, i4), z = load<float>(pz, i4);
        const float ex = eye_x - x, ey = eye_y - y, ez = eye_z - z;
        const float inv_e = inverse(sqrt_ss(length2(ex, ey, ez)));
        const float enx = ex * inv_e, eny = ey * inv_e, enz = ez * inv_e;
        const u32 j4 = i4 + 4;
        const float dx = load<float>(px, j4) - x, dy = load<float>(py, j4) - y, dz = load<float>(pz, j4) - z;
        const float inv_d = inverse(sqrt_ss(length2(dx, dy, dz)));
        t = t + -1.0f;
        const float dnx = dx * inv_d, dny = dy * inv_d, dnz = dz * inv_d;
        const float cx = enz * dny - eny * dnz;
        const float cy = enx * dnz - enz * dnx;
        const float cz = eny * dnx - enx * dny;
        const float inv_c = inverse(sqrt_ss(length2(cx, cy, cz)));
        const float cnx = inv_c * cx, cny = inv_c * cy, cnz = inv_c * cz;
        const float alpha = fade(t, last - t, fade_in, inv_in, fade_out, inv_out) * age_falloff(inv_life * age + -1.0f);
        const float sx = cnx * half_width, sy = cny * half_width, sz = cnz * half_width;
        const u32 n_eye = pack_unit(enx, eny, enz, 127.0f);
        const u32 n_side = pack_unit(cnx, cny, cnz, -127.0f);
        const u32 n_next = pack_unit(dnx, dny, dnz, 127.0f);
        const u32 colour = alpha_channel(alpha * opacity) | r | (g | b);
        put_vertex(pair, x - sx, y - sy, z - sz, n_eye, n_side, n_next, colour, point_packed, offset + u0, v_bits);
        put_vertex(pair + kVertex, negate(negate(sx) - x), negate(negate(sy) - y), negate(negate(sz) - z), n_eye, n_side,
                   n_next, colour, point_packed, u_end + offset, v_bits);
        i4 -= 4;
        o4 -= offset_step;
        pair -= 2 * kVertex;
    } while (i4 != 0xfffffffcu);
}

// sub_2cceec0: a strip along the points' normals. Each point's pair is the
// point plus and minus its normal (nx/ny/nz at the point, or the first entry
// for all with per_point 0) times half_width; the vertices carry
// cross(normal, way), the normal and the way packed, the way being from the
// point to the one at index n + 1 - for every point, as the game has it.
DECOMP_LEAF void sfx_ribbon_along(std::uint8_t* out, u32 n, const float* born, const float* px, const float* py,
                                  const float* pz, float now, float life, float v, float half_width, float u0, float u_span,
                                  float red, float green, const u32* packed, const float* u_offset, const float* nx,
                                  const float* ny, const float* nz, u32 per_point, float blue, float opacity, u32 first,
                                  u32 total, float fade_in, float fade_out) {
    g_along.fetch_add(1, std::memory_order_relaxed);
    const float inv_life = 0.0f >= life ? -1.0f : 1.0f / life;
    const float inv_in = fade_in > 0.0f ? 1.0f / fade_in : 1.0f;
    const float inv_out = fade_out > 0.0f ? 1.0f / fade_out : 1.0f;
    const u32 v_bits = std::bit_cast<u32>(v);
    const float u_end = u0 + u_span;
    const float last = from_u32(total - 1);
    float t = from_u32(first + n);
    const void* offsets = u_offset ? static_cast<const void*>(u_offset) : kNoOffsets;
    const u32 offset_step = u_offset ? 4 : 0;
    const u32 target4 = n * 4 + 4;
    const u32 r = channel(red, 65536.0f, 0xff0000u);
    const u32 g = channel(green, 256.0f, 0xff00u);
    const u32 b = blue_channel(blue);
    u32 i4 = n * 4 - 4;
    u32 k4 = per_point * i4;            // the point's normal
    const u32 normal_step = per_point << 2;
    u32 o4 = (n - 1) * offset_step;
    std::uint8_t* pair = out + static_cast<std::uint64_t>(n + n) * kVertex - 2 * kVertex;
    do {
        const u32 point_packed = at_u32(packed, i4);
        const float offset = std::bit_cast<float>(at_u32(offsets, o4));
        const float age = now - load<float>(born, i4);
        const float x = load<float>(px, i4), y = load<float>(py, i4), z = load<float>(pz, i4);
        const float dx = load<float>(px, target4) - x, dy = load<float>(py, target4) - y, dz = load<float>(pz, target4) - z;
        const float inv_d = inverse(sqrt_ss(length2(dx, dy, dz)));
        t = t + -1.0f;
        const float dnx = dx * inv_d, dny = dy * inv_d, dnz = dz * inv_d;
        const float mz = load<float>(nz, k4), mx = load<float>(nx, k4), my = load<float>(ny, k4);
        const float cx = my * dnz - mz * dny;
        const float cy = mz * dnx - mx * dnz;
        const float cz = mx * dny - my * dnx;
        const float inv_c = inverse(sqrt_ss(length2(cx, cy, cz)));
        const float cnx = inv_c * cx, cny = inv_c * cy, cnz = inv_c * cz;
        const float alpha = fade(t, last - t, fade_in, inv_in, fade_out, inv_out) * age_falloff(inv_life * age + -1.0f);
        const float sx = mx * half_width, sy = my * half_width, sz = mz * half_width;
        const u32 n_side = pack_unit(cnx, cny, cnz, -127.0f);
        const u32 n_normal = pack_unit(mx, my, mz, 127.0f);
        const u32 n_way = pack_unit(dnx, dny, dnz, 127.0f);
        const u32 colour = alpha_channel(alpha * opacity) | r | (g | b);
        put_vertex(pair, x + sx, y + sy, z + sz, n_side, n_normal, n_way, colour, point_packed, offset + u0, v_bits);
        put_vertex(pair + kVertex, negate(sx - x), negate(sy - y), negate(sz - z), n_side, n_normal, n_way, colour,
                   point_packed, u_end + offset, v_bits);
        i4 -= 4;
        k4 -= normal_step;
        o4 -= offset_step;
        pair -= 2 * kVertex;
    } while (i4 != 0xfffffffcu);
}

// ---- Compare runs: the game's version writes, ours must have said what ----

namespace {

using FacingFn = GUEST_ABI void (*)(std::uint8_t*, u32, const float*, const float*, const float*, const float*, float, float,
                                    float, float, float, float, float, float, const u32*, const float*, float, float, float,
                                    float, float, u32, u32, float, float);
using AlongFn = GUEST_ABI void (*)(std::uint8_t*, u32, const float*, const float*, const float*, const float*, float, float,
                                   float, float, float, float, float, float, const u32*, const float*, const float*,
                                   const float*, const float*, u32, float, float, u32, u32, float, float);

void* g_game_facing = nullptr;
void* g_game_along = nullptr;
DecompCompare g_cmp_facing, g_cmp_along;

// Larger than any strip's tail (one to three points); past it a call is only
// the game's.
constexpr u32 kCompareMax = 64;

void count(DecompCompare& c, const char* fn, const std::uint8_t* theirs, const std::vector<std::uint8_t>& ours, u32 n, u32 first,
           u32 total) {
    c.calls.fetch_add(1, std::memory_order_relaxed);
    if (std::memcmp(theirs, ours.data(), ours.size()) == 0) return;
    if (c.differ.fetch_add(1, std::memory_order_relaxed) >= 8) return;
    std::size_t k = 0;
    while (k + 4 <= ours.size() && std::memcmp(theirs + k, ours.data() + k, 4) == 0) k += 4;
    u32 a = 0, b = 0;
    std::memcpy(&a, theirs + k, 4);
    std::memcpy(&b, ours.data() + k, 4);
    host_log("decomp: %s(n %u, first %u of %u) differs at vertex %zu +0x%zx: the game's 0x%08x, ours 0x%08x", fn, n, first, total,
             k / kVertex, k % kVertex, a, b);
}

GUEST_ABI void compare_facing(std::uint8_t* out, u32 n, const float* born, const float* px, const float* py, const float* pz,
                              float now, float eye_x, float eye_y, float eye_z, float life, float v, float half_width, float u0,
                              const u32* packed, const float* u_offset, float u_span, float red, float green, float blue,
                              float opacity, u32 first, u32 total, float fade_in, float fade_out) {
    const auto game = reinterpret_cast<FacingFn>(g_game_facing);
    if (n == 0 || n > kCompareMax) {
        game(out, n, born, px, py, pz, now, eye_x, eye_y, eye_z, life, v, half_width, u0, packed, u_offset, u_span, red, green,
             blue, opacity, first, total, fade_in, fade_out);
        return;
    }
    std::vector<std::uint8_t> ours(static_cast<std::size_t>(n) * 2 * kVertex);
    sfx_ribbon_facing(ours.data(), n, born, px, py, pz, now, eye_x, eye_y, eye_z, life, v, half_width, u0, packed, u_offset,
                      u_span, red, green, blue, opacity, first, total, fade_in, fade_out);
    // The game's version must not fault in its writes: on Windows that is the
    // crash above. A release is what those faults would have done.
    write_watch_release(out, ours.size());
    game(out, n, born, px, py, pz, now, eye_x, eye_y, eye_z, life, v, half_width, u0, packed, u_offset, u_span, red, green, blue,
         opacity, first, total, fade_in, fade_out);
    count(g_cmp_facing, "sub_2cce7b0", out, ours, n, first, total);
}

GUEST_ABI void compare_along(std::uint8_t* out, u32 n, const float* born, const float* px, const float* py, const float* pz,
                             float now, float life, float v, float half_width, float u0, float u_span, float red, float green,
                             const u32* packed, const float* u_offset, const float* nx, const float* ny, const float* nz,
                             u32 per_point, float blue, float opacity, u32 first, u32 total, float fade_in, float fade_out) {
    const auto game = reinterpret_cast<AlongFn>(g_game_along);
    if (n == 0 || n > kCompareMax) {
        game(out, n, born, px, py, pz, now, life, v, half_width, u0, u_span, red, green, packed, u_offset, nx, ny, nz, per_point,
             blue, opacity, first, total, fade_in, fade_out);
        return;
    }
    std::vector<std::uint8_t> ours(static_cast<std::size_t>(n) * 2 * kVertex);
    sfx_ribbon_along(ours.data(), n, born, px, py, pz, now, life, v, half_width, u0, u_span, red, green, packed, u_offset, nx, ny,
                     nz, per_point, blue, opacity, first, total, fade_in, fade_out);
    write_watch_release(out, ours.size());
    game(out, n, born, px, py, pz, now, life, v, half_width, u0, u_span, red, green, packed, u_offset, nx, ny, nz, per_point, blue,
         opacity, first, total, fade_in, fade_out);
    count(g_cmp_along, "sub_2cceec0", out, ours, n, first, total);
}

// push rbp, r15, r14, r13, r12, rbx; sub rsp, 0x30 (0x20): 14 bytes each.
constexpr std::uint8_t kFacingEntry[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x30};
constexpr std::uint8_t kAlongEntry[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x20};

void report() {
    host_log("decomp: the effect ribbons' last points: %llu strips facing the eye, %llu along normals",
             static_cast<ull>(g_facing.load()), static_cast<ull>(g_along.load()));
}

}  // namespace

void decomp_sfx_ribbons_add() {
    const char* area = "sfx";
    DecompFunction facing{"sub_2cce7b0", area, 0x2cce7b0, kFacingEntry, sizeof(kFacingEntry),
                          reinterpret_cast<void*>(&sfx_ribbon_facing), DecompKind::Leaf, &g_game_facing,
                          reinterpret_cast<void*>(&compare_facing), &g_cmp_facing};
    facing.report = &report;
    decomp_add(facing);
    decomp_add({"sub_2cceec0", area, 0x2cceec0, kAlongEntry, sizeof(kAlongEntry), reinterpret_cast<void*>(&sfx_ribbon_along),
                DecompKind::Leaf, &g_game_along, reinterpret_cast<void*>(&compare_along), &g_cmp_along});
}
