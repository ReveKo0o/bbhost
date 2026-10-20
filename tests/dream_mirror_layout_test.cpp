// engine/dream_mirror_layout: the Hunter's Dream layout rewrite on the dump's
// own file - the input and the result are checked by SHA-256 inside, and here
// the part at slot 742 is the mirror's character as a live Enemy.
#include "engine/dream_mirror_layout.h"
#include "gcn/container.h"
#include "test_app0.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main() {
    const std::string path = test_app0_file("dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx");
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("skip: no %s\n", path.c_str());
        return kTestSkip;
    }
    const std::vector<std::uint8_t> dcx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string why;
    std::vector<std::uint8_t> msb = gcn::dcx_decompress(dcx, &why);
    if (msb.empty()) {
        std::printf("FAIL: %s does not inflate: %s\n", path.c_str(), why.c_str());
        return 1;
    }
    if (!dream_mirror_layout(msb, &why)) {
        std::printf("FAIL: %s\n", why.c_str());
        return 1;
    }
    // The parts list is the fourth: walk the list headers to its table.
    const auto rd32 = [&](std::size_t at) {
        std::int32_t v;
        std::memcpy(&v, msb.data() + at, 4);
        return v;
    };
    const auto rd64 = [&](std::size_t at) {
        std::int64_t v;
        std::memcpy(&v, msb.data() + at, 8);
        return v;
    };
    std::size_t at = 0x10;
    for (int k = 0; k < 3; ++k) at = static_cast<std::size_t>(rd64(at + 16 + 8 * static_cast<std::size_t>(rd32(at + 4) - 1)));
    const std::size_t part = static_cast<std::size_t>(rd64(at + 16 + 8 * 742));
    const std::size_t entity = part + static_cast<std::size_t>(rd64(part + 0xb0));
    const std::size_t typedata = part + static_cast<std::size_t>(rd64(part + 0xb8));
    const std::int32_t type = rd32(part + 0x14), index = rd32(part + 0x18), id = rd32(entity), talk = rd32(typedata + 0x10);
    if (type != 2 || index != 6 || id != 2100202 || talk != 210696) {
        std::printf("FAIL: slot 742 is type %d index %d entity %d talk %d\n", type, index, id, talk);
        return 1;
    }
    // The game reads it back compressed the way its own files are.
    const std::vector<std::uint8_t> packed = gcn::dcx_compress(msb);
    if (gcn::dcx_decompress(packed, &why) != msb) {
        std::printf("FAIL: the DCX round trip differs\n");
        return 1;
    }
    std::printf("ok: the mirror's character is a live Enemy at slot 742 (entity %d, talk t%d)\n", id, talk);
    return 0;
}
