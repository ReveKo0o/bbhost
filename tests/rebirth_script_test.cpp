// engine/rebirth_script on the dump's Altar of Despair script - the input and
// the result are checked by SHA-256 inside; here the result holds the new entry
// and states, and the archive with it packs and reads back.
#include "engine/esd.h"
#include "engine/rebirth_script.h"
#include "gcn/container.h"
#include "test_app0.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main() {
    const std::string path = test_app0_file("dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx");
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("skip: no %s\n", path.c_str());
        return kTestSkip;
    }
    const std::vector<std::uint8_t> dcx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string why;
    gcn::Bnd4Archive a;
    if (!gcn::bnd4_read(gcn::dcx_decompress(dcx, &why), a, &why)) {
        std::printf("FAIL: %s does not read: %s\n", path.c_str(), why.c_str());
        return 1;
    }
    gcn::Bnd4File* altar = nullptr;
    for (gcn::Bnd4File& file : a.files) {
        std::string name;
        for (std::size_t i = 0; i + 1 < file.name.size(); i += 2) name.push_back(static_cast<char>(file.name[i]));
        if (name.size() >= 11 && name.compare(name.size() - 11, 11, "t242307.esd") == 0) altar = &file;
    }
    if (!altar) {
        std::printf("FAIL: no t242307.esd in %s\n", path.c_str());
        return 1;
    }
    if (!rebirth_script(altar->data, &why)) {
        std::printf("FAIL: %s\n", why.c_str());
        return 1;
    }
    esd::Script s;
    if (!esd::parse(altar->data, s, &why)) {
        std::printf("FAIL: the result does not parse: %s\n", why.c_str());
        return 1;
    }
    const esd::Group* build = s.group(2147483640);
    const esd::Group* handle = s.group(2147483641);
    if (!build || !handle || handle->states.size() != 16) {
        std::printf("FAIL: the altar's groups are not as written\n");
        return 1;
    }
    // The talk list's second half: the new entry (index 3, the option's text) first.
    const int rest = s.state_in(*build, 3);
    const auto& entry = s.runs[static_cast<std::size_t>(s.states[static_cast<std::size_t>(rest)].entry)].items;
    const esd::Cmd& add = s.cmds[static_cast<std::size_t>(entry.at(0))];
    const auto& args = s.runs[static_cast<std::size_t>(add.args)].items;
    const std::vector<std::uint8_t> want = esd::done(esd::lit(rebirth::kTextOption));
    if (entry.size() != 2 || add.id != 19 || args.size() != 3 || s.exprs[static_cast<std::size_t>(args[1])].code != want) {
        std::printf("FAIL: the option is not the list's new entry\n");
        return 1;
    }
    std::vector<std::uint8_t> packed = gcn::bnd4_pack(a);
    gcn::Bnd4Archive back;
    if (!gcn::bnd4_read(packed, back, &why) || back.files.size() != a.files.size()) {
        std::printf("FAIL: the archive does not read back: %s\n", why.c_str());
        return 1;
    }
    std::printf("ok: the altar offers Rebirth in the Nightmare (%zu-byte script, %zu-byte archive)\n", altar->data.size(), packed.size());
    return 0;
}
