#include "host/tess_lds.h"

#include <algorithm>
#include <cstdio>

std::string tess_lds_where(std::uint64_t ring, std::uint64_t ring_bytes, std::uint64_t region_bytes, const TessLdsUse* uses,
                           std::size_t n, std::uint64_t addr, std::uint64_t precision) {
    using ull = unsigned long long;
    char buf[256];
    const std::uint64_t end = ring + ring_bytes;
    if (addr >= ring && addr < end) {
        std::snprintf(buf, sizeof(buf), "inside the tessellation LDS ring (0x%llx +0x%llx): region %llu at +0x%llx", static_cast<ull>(ring),
                      static_cast<ull>(ring_bytes), static_cast<ull>(region_bytes ? (addr - ring) / region_bytes : 0),
                      static_cast<ull>(region_bytes ? (addr - ring) % region_bytes : addr - ring));
    } else {
        std::snprintf(buf, sizeof(buf), "0x%llx %s the tessellation LDS ring (0x%llx +0x%llx)",
                      static_cast<ull>(addr >= end ? addr - end : ring - addr), addr >= end ? "past the end of" : "before",
                      static_cast<ull>(ring), static_cast<ull>(ring_bytes));
    }
    std::string out = buf;
    // The draw whose LDS holds the faulting range, newest first, or else the
    // one whose LDS it lies closest past the end of.
    const TessLdsUse* in = nullptr;
    const TessLdsUse* after = nullptr;
    std::uint64_t after_by = ~0ull;
    for (std::size_t k = 0; k < n && !in; ++k) {
        const TessLdsUse& u = uses[k];
        if (!u.bytes) continue;
        if (addr + std::max<std::uint64_t>(precision, 1) > u.address && addr < u.address + u.bytes) {
            in = &u;
        } else if (addr >= u.address + u.bytes && addr - (u.address + u.bytes) < after_by) {
            after = &u;
            after_by = addr - (u.address + u.bytes);
        }
    }
    if (in) {
        std::snprintf(buf, sizeof(buf), "; in the LDS of the tessellated draw %llu (0x%llx +0x%llx)", static_cast<ull>(in->draw),
                      static_cast<ull>(in->address), static_cast<ull>(in->bytes));
        out += buf;
        if (in->window && addr >= in->address) {
            std::snprintf(buf, sizeof(buf), ", patch %llu's window of %u bytes at +0x%llx", static_cast<ull>((addr - in->address) / in->window),
                          in->window, static_cast<ull>((addr - in->address) % in->window));
            out += buf;
        }
    } else if (after && after_by < (1ull << 33)) {
        // A 32-bit offset from a window's base reaches at most 4 GiB past it.
        std::snprintf(buf, sizeof(buf), "; 0x%llx past the end of the LDS of the tessellated draw %llu (0x%llx +0x%llx%s)",
                      static_cast<ull>(after_by), static_cast<ull>(after->draw), static_cast<ull>(after->address),
                      static_cast<ull>(after->bytes), after->window ? ", a window a patch" : "");
        out += buf;
    }
    return out;
}
