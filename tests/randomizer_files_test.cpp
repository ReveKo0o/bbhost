// The randomizer's file code against the game dump: every AI bundle, its goal
// and global lists rebuild byte for byte and every program's code is read;
// every layout of the game rebuilds byte for byte, and one with two models
// inserted still names, part for part, the models it did. Skips when the dump
// is absent.
#include "test_app0.h"
#include "gcn/container.h"

#include "../plugins/randomizer/randomizer.cpp"

#include <cstdio>
#include <filesystem>

namespace {

std::vector<std::uint8_t> game_file_at(const std::string& path) {
    std::vector<std::uint8_t> raw;
    if (!gcn::read_file(path, raw)) return {};
    std::string err;
    return gcn::dcx_decompress(raw, &err);
}

std::vector<std::string> model_names_of(const std::vector<std::uint8_t>& b, const std::vector<std::uint64_t>& models) {
    std::vector<std::string> names;
    for (const std::uint64_t m : models) names.push_back(utf16_at(b, m + rd<std::uint64_t>(b, m)));
    return names;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = test_app0_file("dvdroot_ps4");
    std::error_code ec;
    if (!fs::is_directory(root / "script", ec) || !fs::is_directory(root / "map", ec)) {
        std::printf("randomizer_files_test skipped: no dump at %s\n", root.string().c_str());
        return kTestSkip;
    }
    int bad = 0, bundles = 0, programs = 0, layouts = 0;
    for (const auto& e : fs::directory_iterator(root / "script")) {
        const std::string path = e.path().string();
        if (!ends_with(path, ".luabnd.dcx")) continue;
        const std::vector<std::uint8_t> raw = game_file_at(path);
        Bnd bnd;
        if (!bnd_read(raw, bnd) || bnd_write(bnd) != raw) {
            std::printf("bundle does not rebuild: %s\n", path.c_str());
            ++bad;
            continue;
        }
        ++bundles;
        for (const BndFile& f : bnd.files) {
            std::vector<Goal> goals;
            std::vector<std::string> names;
            LuaGlobals lg;
            if (ends_with(f.base, ".luainfo") && !(info_read(f.data, goals) && info_write(goals) == f.data)) {
                std::printf("goal list does not rebuild: %s\n", path.c_str());
                ++bad;
            }
            if (ends_with(f.base, ".luagnl") && !(gnl_read(f.data, names) && gnl_write(names) == f.data)) {
                std::printf("global list does not rebuild: %s\n", path.c_str());
                ++bad;
            }
            if (ends_with(f.base, ".lua")) {
                ++programs;
                if (!lua_globals(f.data, lg)) {
                    std::printf("program not read: %s %s\n", path.c_str(), f.base.c_str());
                    ++bad;
                }
            }
        }
    }
    for (const auto& e : fs::recursive_directory_iterator(root / "map")) {
        const std::string path = e.path().string();
        if (!ends_with(path, ".msb.dcx")) continue;
        const std::vector<std::uint8_t> raw = game_file_at(path);
        std::vector<std::uint8_t> again = raw;
        if (raw.empty() || !msb_add_models(again, {}) || again != raw) {
            std::printf("layout does not rebuild: %s\n", path.c_str());
            ++bad;
            continue;
        }
        ++layouts;
        std::vector<std::uint64_t> before[4], after[4];
        std::vector<std::uint8_t> b = raw;
        if (!msb_lists(raw, before) || !msb_add_models(b, {"c1260", "c0000"}) || !msb_lists(b, after)) {
            std::printf("layout does not take models: %s\n", path.c_str());
            ++bad;
            continue;
        }
        const auto was = model_names_of(raw, before[0]), now = model_names_of(b, after[0]);
        bool ok = after[3].size() == before[3].size() && now.size() == was.size() + 2;
        for (std::size_t i = 0; ok && i < before[3].size(); ++i) {
            const auto x = rd<std::int32_t>(raw, before[3][i] + 0x1c), y = rd<std::int32_t>(b, after[3][i] + 0x1c);
            if (x < 0) {
                ok = y == x;  // a part without a model stays so
                continue;
            }
            ok = y >= 0 && static_cast<std::size_t>(y) < now.size() && was[static_cast<std::size_t>(x)] == now[static_cast<std::size_t>(y)];
        }
        if (!ok) {
            std::printf("parts name other models after an insert: %s\n", path.c_str());
            ++bad;
        }
    }
    std::printf("randomizer_files_test: %d bundles, %d programs, %d layouts; %d problems\n", bundles, programs, layouts, bad);
    return bad == 0 && bundles > 0 && layouts > 0 ? 0 : 1;
}
