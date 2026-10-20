#include "replay/compare.h"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace replay {
namespace {

enum class Enc { Unorm, Snorm, Uint, Sint, Float, Half, B10G11R11, Pack2101010, R5G6B5, D24, Compressed };

struct Layout {
    std::uint32_t bytes = 0;  // per texel, or per block for compressed formats
    int channels = 0;
    Enc enc = Enc::Unorm;
    int bits = 0;             // per channel for the plain encodings
    std::uint32_t block = 1;  // texels per block side
};

bool layout_of(std::uint32_t f, std::uint32_t aspect, Layout& l) {
    const auto plain = [&](int channels, Enc enc, int bits) {
        l = {static_cast<std::uint32_t>(channels * bits / 8), channels, enc, bits, 1};
        return aspect == kAspectColor;
    };
    const auto bc = [&](std::uint32_t bytes) {
        l = {bytes, 0, Enc::Compressed, 0, 4};
        return aspect == kAspectColor;
    };
    switch (f) {
    case VK_FORMAT_R5G6B5_UNORM_PACK16: l = {2, 3, Enc::R5G6B5, 0, 1}; return aspect == kAspectColor;
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: return plain(1, Enc::Unorm, 8);
    case VK_FORMAT_R8_SNORM: return plain(1, Enc::Snorm, 8);
    case VK_FORMAT_R8_UINT: return plain(1, Enc::Uint, 8);
    case VK_FORMAT_R8_SINT: return plain(1, Enc::Sint, 8);
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SRGB: return plain(2, Enc::Unorm, 8);
    case VK_FORMAT_R8G8_SNORM: return plain(2, Enc::Snorm, 8);
    case VK_FORMAT_R8G8_UINT: return plain(2, Enc::Uint, 8);
    case VK_FORMAT_R8G8_SINT: return plain(2, Enc::Sint, 8);
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return plain(4, Enc::Unorm, 8);
    case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_B8G8R8A8_SNORM: return plain(4, Enc::Snorm, 8);
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT: return plain(4, Enc::Uint, 8);
    case VK_FORMAT_R8G8B8A8_SINT: return plain(4, Enc::Sint, 8);
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        l = {4, 4, Enc::Pack2101010, 0, 1};
        return aspect == kAspectColor;
    case VK_FORMAT_R16_UNORM: return plain(1, Enc::Unorm, 16);
    case VK_FORMAT_R16_SNORM: return plain(1, Enc::Snorm, 16);
    case VK_FORMAT_R16_UINT: return plain(1, Enc::Uint, 16);
    case VK_FORMAT_R16_SINT: return plain(1, Enc::Sint, 16);
    case VK_FORMAT_R16_SFLOAT: return plain(1, Enc::Half, 16);
    case VK_FORMAT_R16G16_UNORM: return plain(2, Enc::Unorm, 16);
    case VK_FORMAT_R16G16_SNORM: return plain(2, Enc::Snorm, 16);
    case VK_FORMAT_R16G16_UINT: return plain(2, Enc::Uint, 16);
    case VK_FORMAT_R16G16_SINT: return plain(2, Enc::Sint, 16);
    case VK_FORMAT_R16G16_SFLOAT: return plain(2, Enc::Half, 16);
    case VK_FORMAT_R16G16B16A16_UNORM: return plain(4, Enc::Unorm, 16);
    case VK_FORMAT_R16G16B16A16_SNORM: return plain(4, Enc::Snorm, 16);
    case VK_FORMAT_R16G16B16A16_UINT: return plain(4, Enc::Uint, 16);
    case VK_FORMAT_R16G16B16A16_SINT: return plain(4, Enc::Sint, 16);
    case VK_FORMAT_R16G16B16A16_SFLOAT: return plain(4, Enc::Half, 16);
    case VK_FORMAT_R32_UINT: return plain(1, Enc::Uint, 32);
    case VK_FORMAT_R32_SINT: return plain(1, Enc::Sint, 32);
    case VK_FORMAT_R32_SFLOAT: return plain(1, Enc::Float, 32);
    case VK_FORMAT_R32G32_UINT: return plain(2, Enc::Uint, 32);
    case VK_FORMAT_R32G32_SINT: return plain(2, Enc::Sint, 32);
    case VK_FORMAT_R32G32_SFLOAT: return plain(2, Enc::Float, 32);
    case VK_FORMAT_R32G32B32_UINT: return plain(3, Enc::Uint, 32);
    case VK_FORMAT_R32G32B32_SINT: return plain(3, Enc::Sint, 32);
    case VK_FORMAT_R32G32B32_SFLOAT: return plain(3, Enc::Float, 32);
    case VK_FORMAT_R32G32B32A32_UINT: return plain(4, Enc::Uint, 32);
    case VK_FORMAT_R32G32B32A32_SINT: return plain(4, Enc::Sint, 32);
    case VK_FORMAT_R32G32B32A32_SFLOAT: return plain(4, Enc::Float, 32);
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: l = {4, 3, Enc::B10G11R11, 0, 1}; return aspect == kAspectColor;
    case VK_FORMAT_D16_UNORM: l = {2, 1, Enc::Unorm, 16, 1}; return aspect == kAspectDepth;
    case VK_FORMAT_D32_SFLOAT: l = {4, 1, Enc::Float, 32, 1}; return aspect == kAspectDepth;
    case VK_FORMAT_X8_D24_UNORM_PACK32: l = {4, 1, Enc::D24, 0, 1}; return aspect == kAspectDepth;
    case VK_FORMAT_S8_UINT: l = {1, 1, Enc::Uint, 8, 1}; return aspect == kAspectStencil;
    case VK_FORMAT_D16_UNORM_S8_UINT:
        if (aspect == kAspectStencil) { l = {1, 1, Enc::Uint, 8, 1}; return true; }
        l = {2, 1, Enc::Unorm, 16, 1};
        return aspect == kAspectDepth;
    case VK_FORMAT_D24_UNORM_S8_UINT:
        if (aspect == kAspectStencil) { l = {1, 1, Enc::Uint, 8, 1}; return true; }
        l = {4, 1, Enc::D24, 0, 1};
        return aspect == kAspectDepth;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        if (aspect == kAspectStencil) { l = {1, 1, Enc::Uint, 8, 1}; return true; }
        l = {4, 1, Enc::Float, 32, 1};
        return aspect == kAspectDepth;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: return bc(8);
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return bc(16);
    default: return false;
    }
}

double half_value(std::uint16_t h) {
    const int sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    double v;
    if (exp == 0) v = std::ldexp(static_cast<double>(man), -24);
    else if (exp == 31) v = man ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else v = std::ldexp(static_cast<double>(man | 0x400), exp - 25);
    return sign ? -v : v;
}

// Unsigned small floats of B10G11R11: 5 exponent bits and a 6- or 5-bit mantissa.
double small_float(std::uint32_t bits, int man_bits) {
    const std::uint32_t exp = (bits >> man_bits) & 0x1f, man = bits & ((1u << man_bits) - 1);
    if (exp == 0) return std::ldexp(static_cast<double>(man), -14 - man_bits);
    if (exp == 31) return man ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    return std::ldexp(static_cast<double>(man | (1u << man_bits)), static_cast<int>(exp) - 15 - man_bits);
}

json::Value number_or_string(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    return v;
}

}  // namespace

bool texel_layout(std::uint32_t vk_format, std::uint32_t aspect, std::uint32_t& block_bytes, std::uint32_t& block_w,
                  std::uint32_t& block_h) {
    Layout l;
    if (!layout_of(vk_format, aspect, l)) return false;
    block_bytes = l.bytes;
    block_w = block_h = l.block;
    return true;
}

std::uint64_t subresource_bytes(std::uint32_t vk_format, std::uint32_t aspect, std::uint32_t width, std::uint32_t height,
                                std::uint32_t depth) {
    std::uint32_t bytes = 0, bw = 1, bh = 1;
    if (!texel_layout(vk_format, aspect, bytes, bw, bh)) return 0;
    return static_cast<std::uint64_t>((width + bw - 1) / bw) * ((height + bh - 1) / bh) * std::max(depth, 1u) * bytes;
}

bool decode_plane(std::uint32_t vk_format, std::uint32_t aspect, const std::uint8_t* data, std::size_t bytes,
                  std::uint32_t width, std::uint32_t height, Plane& out, std::string& error) {
    Layout l;
    if (!layout_of(vk_format, aspect, l)) {
        error = "format " + std::to_string(vk_format) + " aspect " + std::to_string(aspect) + " has no known texel layout";
        return false;
    }
    if (l.enc == Enc::Compressed) {
        error = "format " + std::to_string(vk_format) + " is block-compressed; compare it as bytes";
        return false;
    }
    const std::uint64_t want = static_cast<std::uint64_t>(width) * height * l.bytes;
    if (bytes != want) {
        error = "plane holds " + std::to_string(bytes) + " bytes, " + std::to_string(width) + "x" + std::to_string(height) +
                " of format " + std::to_string(vk_format) + " needs " + std::to_string(want);
        return false;
    }
    out.width = width;
    out.height = height;
    out.channels = l.channels;
    out.values.resize(static_cast<std::size_t>(width) * height * l.channels);
    const std::size_t texels = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < texels; ++i) {
        const std::uint8_t* p = data + i * l.bytes;
        double* v = out.values.data() + i * l.channels;
        switch (l.enc) {
        case Enc::Unorm:
        case Enc::Snorm:
        case Enc::Uint:
        case Enc::Sint:
            for (int c = 0; c < l.channels; ++c) {
                std::uint32_t u = 0;
                std::int32_t s = 0;
                if (l.bits == 8) {
                    u = p[c];
                    s = static_cast<std::int8_t>(p[c]);
                } else if (l.bits == 16) {
                    std::uint16_t w;
                    std::memcpy(&w, p + c * 2, 2);
                    u = w;
                    s = static_cast<std::int16_t>(w);
                } else {
                    std::memcpy(&u, p + c * 4, 4);
                    s = static_cast<std::int32_t>(u);
                }
                const double max_u = std::ldexp(1.0, l.bits) - 1.0, max_s = std::ldexp(1.0, l.bits - 1) - 1.0;
                v[c] = l.enc == Enc::Unorm ? u / max_u
                     : l.enc == Enc::Snorm ? std::max(s / max_s, -1.0)
                     : l.enc == Enc::Uint  ? static_cast<double>(u)
                                           : static_cast<double>(s);
            }
            break;
        case Enc::Float:
            for (int c = 0; c < l.channels; ++c) {
                float f;
                std::memcpy(&f, p + c * 4, 4);
                v[c] = f;
            }
            break;
        case Enc::Half:
            for (int c = 0; c < l.channels; ++c) {
                std::uint16_t h;
                std::memcpy(&h, p + c * 2, 2);
                v[c] = half_value(h);
            }
            break;
        case Enc::B10G11R11: {
            std::uint32_t w;
            std::memcpy(&w, p, 4);
            v[0] = small_float(w & 0x7ff, 6);
            v[1] = small_float((w >> 11) & 0x7ff, 6);
            v[2] = small_float(w >> 22, 5);
            break;
        }
        case Enc::Pack2101010: {
            // Fields low to high, whichever way the format names them.
            std::uint32_t w;
            std::memcpy(&w, p, 4);
            v[0] = (w & 0x3ff) / 1023.0;
            v[1] = ((w >> 10) & 0x3ff) / 1023.0;
            v[2] = ((w >> 20) & 0x3ff) / 1023.0;
            v[3] = (w >> 30) / 3.0;
            break;
        }
        case Enc::R5G6B5: {
            std::uint16_t w;
            std::memcpy(&w, p, 2);
            v[0] = (w & 0x1f) / 31.0;
            v[1] = ((w >> 5) & 0x3f) / 63.0;
            v[2] = (w >> 11) / 31.0;
            break;
        }
        case Enc::D24: {
            std::uint32_t w;
            std::memcpy(&w, p, 4);
            v[0] = (w & 0xffffff) / 16777215.0;
            break;
        }
        case Enc::Compressed: break;
        }
    }
    return true;
}

bool values_match(double a, double b, const Tolerance& t) {
    const bool fa = std::isfinite(a), fb = std::isfinite(b);
    if (!fa || !fb) {
        if (fa != fb) return false;
        if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
        return a == b;
    }
    return std::fabs(a - b) <= t.atol + t.rtol * std::max(std::fabs(a), std::fabs(b));
}

Comparison compare_planes(const Plane& a, const Plane& b, const Tolerance& t, std::size_t keep_failures) {
    Comparison c;
    c.width = a.width;
    c.height = a.height;
    c.channels = a.channels;
    c.tolerance = t;
    const std::size_t texels = static_cast<std::size_t>(a.width) * a.height;
    if (a.width != b.width || a.height != b.height || a.channels != b.channels || a.channels <= 0 ||
        a.values.size() != texels * a.channels || b.values.size() != a.values.size()) {
        c.shape_ok = false;
        return c;
    }
    const int ch = a.channels;
    c.texels = texels;
    c.per_channel.resize(static_cast<std::size_t>(ch));
    c.texel_error.assign(texels, 0.0f);
    c.texel_failed.assign(texels, 0);
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * a.width + x;
            float err = 0.0f;
            bool failed = false, different = false;
            for (int k = 0; k < ch; ++k) {
                const double va = a.values[i * ch + k], vb = b.values[i * ch + k];
                ChannelStats& s = c.per_channel[static_cast<std::size_t>(k)];
                const bool fa = std::isfinite(va), fb = std::isfinite(vb);
                s.nonfinite_a += !fa;
                s.nonfinite_b += !fb;
                const bool ok = values_match(va, vb, t);
                if (fa && fb) {
                    const double d = std::fabs(va - vb);
                    if (d > 0.0) different = true;
                    if (d > s.max_abs) {
                        s.max_abs = d;
                        s.max_x = x;
                        s.max_y = y;
                        s.max_a = va;
                        s.max_b = vb;
                    }
                    err = std::max(err, static_cast<float>(d));
                } else if (!ok) {
                    ++s.nonfinite_mismatch;
                    different = true;
                    err = std::numeric_limits<float>::infinity();
                }
                if (!ok) {
                    ++s.failed;
                    failed = true;
                    if (c.first_failures.size() < keep_failures) c.first_failures.push_back({x, y, k, va, vb});
                }
            }
            c.texel_error[i] = err;
            c.texel_failed[i] = failed ? 1 : 0;
            c.texels_failed += failed;
            c.texels_different += different;
        }
    }
    return c;
}

json::Value to_json(const Comparison& c) {
    json::Value v = json::Value::make_object();
    v.set("match", c.match());
    v.set("shape_ok", c.shape_ok);
    v.set("width", c.width);
    v.set("height", c.height);
    v.set("channels", c.channels);
    json::Value tol = json::Value::make_object();
    tol.set("atol", c.tolerance.atol);
    tol.set("rtol", c.tolerance.rtol);
    tol.set("rule", "abs(a-b) <= atol + rtol*max(abs(a),abs(b)); non-finite only equal to the same non-finite value");
    v.set("tolerance", tol);
    v.set("texels", c.texels);
    v.set("texels_failed", c.texels_failed);
    v.set("texels_different", c.texels_different);
    json::Value chans = json::Value::make_array();
    for (std::size_t k = 0; k < c.per_channel.size(); ++k) {
        const ChannelStats& s = c.per_channel[k];
        json::Value o = json::Value::make_object();
        o.set("channel", static_cast<unsigned>(k));
        o.set("failed", s.failed);
        o.set("max_abs_error", s.max_abs);
        json::Value at = json::Value::make_array();
        at.push(s.max_x);
        at.push(s.max_y);
        o.set("max_at", at);
        o.set("max_a", number_or_string(s.max_a));
        o.set("max_b", number_or_string(s.max_b));
        o.set("nonfinite_a", s.nonfinite_a);
        o.set("nonfinite_b", s.nonfinite_b);
        o.set("nonfinite_mismatch", s.nonfinite_mismatch);
        chans.push(o);
    }
    v.set("per_channel", chans);
    json::Value fails = json::Value::make_array();
    for (const Mismatch& m : c.first_failures) {
        json::Value o = json::Value::make_object();
        o.set("x", m.x);
        o.set("y", m.y);
        o.set("channel", m.channel);
        o.set("a", number_or_string(m.a));
        o.set("b", number_or_string(m.b));
        fails.push(o);
    }
    v.set("first_failures", fails);
    return v;
}

bool write_heatmap(const std::string& path, const Comparison& c) {
    if (!c.shape_ok || c.texel_error.size() != static_cast<std::size_t>(c.width) * c.height) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", c.width, c.height);
    double max_err = 0.0;
    for (float e : c.texel_error) {
        if (std::isfinite(e)) max_err = std::max(max_err, static_cast<double>(e));
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(c.width) * 3);
    for (std::uint32_t y = 0; y < c.height; ++y) {
        for (std::uint32_t x = 0; x < c.width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * c.width + x;
            const float e = c.texel_error[i];
            std::uint8_t* px = &row[static_cast<std::size_t>(x) * 3];
            px[0] = px[1] = px[2] = 0;
            if (e == 0.0f && !c.texel_failed[i]) continue;
            // Log scale against the largest finite error in the image.
            const double s = !std::isfinite(e) || max_err <= 0.0 ? 1.0 : std::log1p(e / max_err * 1023.0) / std::log(1024.0);
            if (c.texel_failed[i]) {
                px[0] = static_cast<std::uint8_t>(128 + 127 * s);
                px[1] = static_cast<std::uint8_t>(255 * s * s);
            } else {
                px[2] = static_cast<std::uint8_t>(64 + 191 * s);
            }
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    return std::fclose(f) == 0;
}

}  // namespace replay
