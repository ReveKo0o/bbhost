// Raw texel decoding and tolerance comparison for draw replay. Values are
// compared as
// stored: floats as floats, normalized formats as code / max code, integer
// formats as integers. Nothing is clipped or tone mapped.
#pragma once

#include "replay/json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay {

// VkImageAspectFlagBits values.
enum Aspect : std::uint32_t { kAspectColor = 1, kAspectDepth = 2, kAspectStencil = 4 };

// Bytes per block and block extent of one aspect of a format, laid out as
// vkCmdCopyImageToBuffer writes it. False for formats this does not know.
bool texel_layout(std::uint32_t vk_format, std::uint32_t aspect, std::uint32_t& block_bytes, std::uint32_t& block_w,
                  std::uint32_t& block_h);
// Bytes of one mip level / array layer; 0 when the format is unknown.
std::uint64_t subresource_bytes(std::uint32_t vk_format, std::uint32_t aspect, std::uint32_t width, std::uint32_t height,
                                std::uint32_t depth);

struct Plane {
    std::uint32_t width = 0, height = 0;
    int channels = 0;
    std::vector<double> values;  // row-major, `channels` per texel
};
// Decodes one aspect of an uncompressed format. Compressed formats are inputs
// only and are compared as bytes, not decoded.
bool decode_plane(std::uint32_t vk_format, std::uint32_t aspect, const std::uint8_t* data, std::size_t bytes,
                  std::uint32_t width, std::uint32_t height, Plane& out, std::string& error);

struct Tolerance {
    double atol = 0.0;
    double rtol = 0.0;
};
// abs(a - b) <= atol + rtol * max(abs(a), abs(b)). A non-finite value passes
// only against the same non-finite value (NaN with NaN, equal infinities).
bool values_match(double a, double b, const Tolerance& t);

struct ChannelStats {
    std::uint64_t failed = 0;
    std::uint64_t nonfinite_a = 0, nonfinite_b = 0, nonfinite_mismatch = 0;
    double max_abs = 0.0;  // largest finite |a - b|
    std::uint32_t max_x = 0, max_y = 0;
    double max_a = 0.0, max_b = 0.0;
};
struct Mismatch {
    std::uint32_t x, y;
    int channel;
    double a, b;
};
struct Comparison {
    std::uint32_t width = 0, height = 0;
    int channels = 0;
    Tolerance tolerance;
    bool shape_ok = true;
    std::uint64_t texels = 0, texels_failed = 0, texels_different = 0;
    std::vector<ChannelStats> per_channel;
    std::vector<Mismatch> first_failures;  // row-major order
    // Per texel, the largest |a - b| over its channels; infinity for a
    // non-finite mismatch. Used for the heatmap.
    std::vector<float> texel_error;
    std::vector<std::uint8_t> texel_failed;
    bool match() const { return shape_ok && texels_failed == 0; }
};
Comparison compare_planes(const Plane& a, const Plane& b, const Tolerance& t, std::size_t keep_failures = 32);
json::Value to_json(const Comparison& c);
// Binary PPM: black where equal, blue where different but within tolerance,
// red (larger errors brighter, toward yellow) where the comparison failed.
bool write_heatmap(const std::string& path, const Comparison& c);

}  // namespace replay
