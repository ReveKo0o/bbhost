// engine/esd and gcn::bnd4_read/bnd4_pack on the dump's own talk scripts: every
// archive packs back byte for byte, and every script parses and writes back
// byte for byte (the model keeps what the game's writer does: the initial
// state's copy, the repeated condition list, the name's alignment).
#include "engine/esd.h"
#include "gcn/container.h"
#include "test_app0.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = test_app0_file("dvdroot_ps4/script/talk");
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("skip: no %s\n", dir.string().c_str());
        return kTestSkip;
    }
    int archives = 0, scripts = 0, failed = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().string().find(".talkesdbnd.dcx") == std::string::npos) continue;
        std::ifstream f(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> dcx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string why;
        const std::vector<std::uint8_t> raw = gcn::dcx_decompress(dcx, &why);
        gcn::Bnd4Archive a;
        if (raw.empty() || !gcn::bnd4_read(raw, a, &why)) {
            std::printf("FAIL: %s: %s\n", entry.path().filename().string().c_str(), why.c_str());
            ++failed;
            continue;
        }
        ++archives;
        if (gcn::bnd4_pack(a) != raw) {
            std::printf("FAIL: %s does not pack back as it was\n", entry.path().filename().string().c_str());
            ++failed;
        }
        for (const gcn::Bnd4File& file : a.files) {
            esd::Script s;
            if (!esd::parse(file.data, s, &why)) {
                std::printf("FAIL: a script in %s does not parse: %s\n", entry.path().filename().string().c_str(), why.c_str());
                ++failed;
                continue;
            }
            ++scripts;
            if (esd::serialize(s) != file.data) {
                std::printf("FAIL: a script in %s does not write back as it was\n", entry.path().filename().string().c_str());
                ++failed;
            }
        }
    }
    if (!archives) {
        std::printf("skip: no talk-script archives in %s\n", dir.string().c_str());
        return kTestSkip;
    }
    if (failed) return 1;
    std::printf("ok: %d archives and %d talk scripts write back byte for byte\n", archives, scripts);
    return 0;
}
