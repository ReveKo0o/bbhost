#include "engine/five_players.h"

#include "core/config.h"
#include "core/elf.h"
#include "engine/addr.h"
#include "engine/maiden_events.h"
#include "gcn/container.h"
#include "hle/fs.h"
#include "log.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Bumped whenever the rewrite changes, so installs remake the files.
constexpr const char* kStamp = "five-players 1";

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& p, const std::vector<std::uint8_t>& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string script_path(const char* map) { return std::string("dvdroot_ps4/event/") + map + ".emevd.dcx"; }

}  // namespace

void five_players_install(ElfImage* image) {
    if (!config().five_players) {
        host_log("five players: off (PC enhancements: Five players)");
        return;
    }
    if (!image || image->sha256 != kEboot109Sha256) {
        host_log("five players: off - not the 1.09 eboot");
        return;
    }
    const char* app0 = hle_fs_app0_root();
    const char* data = hle_fs_data_root();
    if (!app0 || !*app0 || !data || !*data) {
        host_log("five players: off - no dump or data folder");
        return;
    }
    const fs::path out = fs::path(data) / "bbhost" / "invasion-assets";
    const auto have = read_file(out / "stamp");
    bool fresh = have && std::string(have->begin(), have->end()) == kStamp;
    for (const MaidenEventMap& m : maiden_event_maps()) fresh = fresh && fs::exists(out / script_path(m.map));
    if (!fresh) {
        int made = 0;
        for (const MaidenEventMap& m : maiden_event_maps()) {
            const std::string rel = script_path(m.map);
            const auto src = read_file(hle_fs_game_file(rel));
            if (!src) {
                host_log("five players: the dump has no %s; that map keeps the game's maidens", rel.c_str());
                continue;
            }
            std::string why;
            std::vector<std::uint8_t> emevd = gcn::dcx_decompress(*src, &why);
            if (emevd.empty() || !maiden_events_rewrite(m.map, emevd, &why)) {
                host_log("five players: %s keeps the game's maidens - %s", m.map, why.c_str());
                continue;
            }
            const std::vector<std::uint8_t> packed = gcn::dcx_compress(emevd);
            if (packed.empty() || !write_file(out / rel, packed)) {
                host_log("five players: cannot write %s", (out / rel).string().c_str());
                continue;
            }
            ++made;
        }
        const std::string stamp = kStamp;
        if (!write_file(out / "stamp", std::vector<std::uint8_t>(stamp.begin(), stamp.end()))) {
            host_log("five players: cannot write %s", out.string().c_str());
            return;
        }
        host_log("five players: rewrote %d of %zu maps' maiden events in %s", made, maiden_event_maps().size(),
                 out.string().c_str());
    }
    // Mounted only with a script in it: a dump whose scripts are not the 1.09
    // ones leaves the folder empty, and an overlay with nothing to serve was
    // one more folder every file the game opened was looked for in.
    std::size_t scripts = 0;
    for (const MaidenEventMap& m : maiden_event_maps()) scripts += fs::exists(out / script_path(m.map)) ? 1 : 0;
    if (!scripts) {
        host_log("five players: off - no map's maiden events could be rewritten from this dump; the maidens are the game's");
        return;
    }
    hle_fs_add_generated_root(out.string().c_str());
    host_log("five players: Mensis, Mergo's Loft, the Nightmare Frontier and the Old Hunters' areas hold two invaders "
             "(%zu of %zu maps' scripts)",
             scripts, maiden_event_maps().size());
}
