// host/tess_lds.h: the device-fault report's line for an address against the
// tessellation LDS ring - inside it (region, draw, patch window), just past a
// draw's LDS, past or before the ring - for a ring and draws laid out as the
// renderer lays them out.
#include "host/tess_lds.h"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;

void expect(const std::string& got, const std::string& want) {
    if (got != want) {
        std::printf("FAIL\n  got  %s\n  want %s\n", got.c_str(), want.c_str());
        ++g_fail;
    }
}

}  // namespace

int main() {
    constexpr std::uint64_t ring = 0x700000000ull, ring_bytes = 16ull * (16 << 20), region = 16 << 20;
    // Newest first: a hull draw in region 0 (3110 patches of 848 bytes), and an
    // emulated one in region 1.
    const TessLdsUse uses[] = {
        {172132, ring + 0x10000, 0x28a000, 848},
        {171000, ring + 0x1000000, 0x5000, 0},
    };
    const auto where = [&](std::uint64_t addr, std::uint64_t precision = 0x1000) {
        return tess_lds_where(ring, ring_bytes, region, uses, 2, addr, precision);
    };

    // Inside the hull draw's LDS: its patch and the offset in that window.
    expect(where(ring + 0x10000 + 848 * 5 + 0x40),
           "inside the tessellation LDS ring (0x700000000 +0x10000000): region 0 at +0x110d0; in the LDS of the tessellated draw 172132 "
           "(0x700010000 +0x28a000), patch 5's window of 848 bytes at +0x40");
    // Inside the ring but past that draw's LDS.
    expect(where(ring + 0x10000 + 0x28a000 + 0x2000),
           "inside the tessellation LDS ring (0x700000000 +0x10000000): region 0 at +0x29c000; 0x2000 past the end of the LDS of the "
           "tessellated draw 172132 (0x700010000 +0x28a000, a window a patch)");
    // Past the ring: how far, and the draw whose LDS ends closest before it.
    expect(where(ring + ring_bytes + 0x40000000),
           "0x40000000 past the end of the tessellation LDS ring (0x700000000 +0x10000000); 0x4effb000 past the end of the LDS of the "
           "tessellated draw 171000 (0x701000000 +0x5000)");
    // Before the ring.
    expect(where(ring - 0x1000000), "0x1000000 before the tessellation LDS ring (0x700000000 +0x10000000)");
    // A fault page that only overlaps the start of a draw's LDS is in it.
    expect(where(ring + 0x10000 - 0x800),
           "inside the tessellation LDS ring (0x700000000 +0x10000000): region 0 at +0xf800; in the LDS of the tessellated draw 172132 "
           "(0x700010000 +0x28a000)");
    // Further past every draw than a 32-bit offset reaches: the ring alone.
    expect(where(ring + ring_bytes + 0x300000000ull), "0x300000000 past the end of the tessellation LDS ring (0x700000000 +0x10000000)");
    // A region of the second size, and no draws yet.
    expect(tess_lds_where(ring, ring_bytes, region, nullptr, 0, ring + region * 3 + 0x20, 0x1000),
           "inside the tessellation LDS ring (0x700000000 +0x10000000): region 3 at +0x20");

    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("tess_lds_test: ok\n");
    return 0;
}
