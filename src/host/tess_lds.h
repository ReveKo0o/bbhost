#pragma once

// Where a device fault's address lies against the tessellation LDS ring
// (render.cpp): the buffer the tessellated draws keep their control points
// in, the game's own hull draws a window a patch. Inside it (which region,
// which draw's LDS, which patch's window), or how far past or before it - the
// device-fault report's line for an address no import holds. No Vulkan here.

#include <cstddef>
#include <cstdint>
#include <string>

struct TessLdsUse {
    std::uint64_t draw = 0;  // the draw recorded next, as the GPU's markers number draws
    std::uint64_t address = 0, bytes = 0;
    std::uint32_t window = 0;  // bytes a patch (the game's own hull), 0 for the emulated path
};

// `uses` newest first. `precision`: the fault names a range that long from
// `addr` (a page, from VK_EXT_device_fault).
std::string tess_lds_where(std::uint64_t ring, std::uint64_t ring_bytes, std::uint64_t region_bytes, const TessLdsUse* uses,
                           std::size_t n, std::uint64_t addr, std::uint64_t precision);
