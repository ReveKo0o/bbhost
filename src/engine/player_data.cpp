#include "engine/player_data.h"

#include "core/elf.h"
#include "core/portable.h"
#include "engine/addr.h"
#include "engine/menu_pointer.h"
#include "bbhost/engine/sprj/game_data_man.hpp"
#include "net/discord_presence.h"
#include "log.h"

#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace {

constexpr std::uint64_t kGameDataMan = 0x593b130;  // GameDataMan*; the local record at +0x08

struct Stat {
    const char* name;
    bool manager;  // GameDataMan's own field, else the local player's record
    std::uint32_t offset;
    bool is_signed;
};
// The record: GameDataManPlayerRecord, 32-bit. The manager's counters (from
// the saturating add helpers 0x18eb5c0..0x18eb660 and the save reader):
// unsigned 32-bit but ClearCount.
constexpr Stat kStats[] = {
    {"hp", false, 0x14, true},          {"max_hp", false, 0x18, true},   {"stamina", false, 0x30, true},
    {"max_stamina", false, 0x34, true}, {"vitality", false, 0x40, true}, {"endurance", false, 0x48, true},
    {"strength", false, 0x58, true},    {"skill", false, 0x60, true},    {"bloodtinge", false, 0x68, true},
    {"arcane", false, 0x70, true},      {"insight", false, 0x84, true},  {"level", false, 0x90, true},
    {"echoes", false, 0x94, true},
    {"ng_cycle", true, 0x68, true},     {"coop_helps", true, 0x78, false}, {"invader_kills", true, 0x7c, false},
    {"true_deaths", true, 0x84, false}, {"deaths", true, 0x88, false},     {"play_time_ms", true, 0x94, false},
};

std::uint64_t g_slide = 0;
bool g_ok = false;
std::string g_last_logged_identity;

std::uint64_t manager() {
    std::uint64_t man = 0;
    const auto at = g_slide + (kGameDataMan - kPreferredGuestSlide);
    if (!g_ok || !host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), &man, 8)) return 0;
    return man;
}

std::uint64_t record() {
    const std::uint64_t man = manager();
    std::uint64_t rec = 0;
    if (!man || !host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(man + 8)), &rec, 8)) return 0;
    return rec;
}

// Where a stat lives, or 0 when no character is loaded (the manager's
// counters are read only while there is a record, so both mean the same).
std::uint64_t base_of(const Stat& s) {
    const std::uint64_t rec = record();
    return rec ? (s.manager ? manager() : rec) : 0;
}

const Stat* find(const char* name) {
    if (!name) return nullptr;
    for (const Stat& s : kStats) {
        if (std::strcmp(s.name, name) == 0) return &s;
    }
    return nullptr;
}

}  // namespace

void player_data_install(ElfImage* image) {
    g_slide = image->mem.slide;
    g_ok = image->sha256 == kEboot109Sha256;
}

bool player_stat_get(const char* name, std::int64_t* value) {
    const Stat* s = find(name);
    const std::uint64_t base = s ? base_of(*s) : 0;
    std::uint32_t v = 0;
    if (!base || !host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(base + s->offset)), &v, 4)) return false;
    if (value) *value = s->is_signed ? static_cast<std::int64_t>(static_cast<std::int32_t>(v)) : static_cast<std::int64_t>(v);
    return true;
}

bool player_stat_set(const char* name, std::int64_t value) {
    const Stat* s = find(name);
    const std::uint64_t base = s ? base_of(*s) : 0;
    if (!base) return false;
    const std::uint32_t v = static_cast<std::uint32_t>(value);
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(base + s->offset)), &v, 4);
    return true;
}

bool player_origin(int* origin) {
    const std::uint64_t rec = record();
    std::uint8_t v = 0;
    if (!rec || !host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(rec + 0xce)), &v, 1)) return false;
    if (origin) *origin = v;
    return true;
}

namespace {

std::string utf16_to_utf8(const std::uint16_t* text, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count && text[i]; ++i) {
        std::uint32_t cp = text[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < count && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (text[++i] - 0xdc00);
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            continue;
        }
        if (cp <= 0x7f) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

}  // namespace

bool player_character_identity(std::string* name, int* level) {
    const std::uint64_t man = manager();
    const std::uint64_t rec = record();
    if (!man || !rec) return false;

    std::uint32_t live_level = 0;
    std::uint32_t live_play_time_ms = 0;
    std::uint64_t summaries = 0;
    if (!host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(rec + bb::GAME_DATA_MAN_PLAYER_RECORD_LEVEL_OFFSET)),
                        &live_level, sizeof(live_level)) ||
        !host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(man + 0x94)), &live_play_time_ms,
                        sizeof(live_play_time_ms)) ||
        !host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(man + 0x58)), &summaries,
                        sizeof(summaries)) ||
        !summaries || live_level == 0 || live_level > 544) {
        return false;
    }

    // The native save summary stores play time in seconds; the live manager
    // holds the same counter in milliseconds. Require a unique close match so
    // a different save with the same level is never guessed as the active one.
    const std::uint32_t live_seconds = live_play_time_ms / 1000;
    int match = -1;
    for (std::size_t i = 0; i < bb::GAME_DATA_CHARACTER_SUMMARY_COUNT; ++i) {
        const std::uint8_t occupied_offset = static_cast<std::uint8_t>(0x10 + i);
        std::uint8_t occupied = 0;
        if (!host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(summaries + occupied_offset)),
                            &occupied, sizeof(occupied)) || !occupied) {
            continue;
        }
        const std::uint64_t summary = summaries + 0x20 + i * bb::GAME_DATA_CHARACTER_SUMMARY_SIZE;
        std::uint32_t summary_level = 0, summary_time = 0;
        if (!host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(summary + 0x24)), &summary_level,
                            sizeof(summary_level)) ||
            !host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(summary + 0x28)), &summary_time,
                            sizeof(summary_time))) {
            return false;
        }
        const std::uint32_t time_delta = summary_time > live_seconds ? summary_time - live_seconds : live_seconds - summary_time;
        if (summary_level == live_level && time_delta <= 10) {
            if (match >= 0) return false;  // ambiguous summaries are deliberately not guessed
            match = static_cast<int>(i);
        }
    }
    if (match < 0) return false;

    const std::uint64_t summary = summaries + 0x20 + static_cast<std::size_t>(match) * bb::GAME_DATA_CHARACTER_SUMMARY_SIZE;
    std::array<std::uint16_t, 17> name_units{};
    if (!host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(summary)), name_units.data(),
                        name_units.size() * sizeof(name_units[0]))) {
        return false;
    }
    std::string decoded = utf16_to_utf8(name_units.data(), name_units.size());
    if (decoded.empty()) return false;
    if (name) *name = std::move(decoded);
    if (level) *level = static_cast<int>(live_level);
    return true;
}

void player_data_tick() {
    if (!g_ok) return;
    if (menu_pointer_front_end()) {
        net::UpdateDiscordMainMenuPresence();
        g_last_logged_identity.clear();
        return;
    }
    std::string character_name;
    int level = 0;
    if (player_character_identity(&character_name, &level)) {
        net::UpdateDiscordGamePresence(character_name.c_str(), level);
        const std::string identity = character_name + "#" + std::to_string(level);
        if (identity == g_last_logged_identity) return;
        g_last_logged_identity = identity;
    } else {
        return;  // wait until the active save can be matched without guessing
    }
    char line[512];
    std::size_t n = 0;
    for (const Stat& s : kStats) {
        std::int64_t v = 0;
        if (player_stat_get(s.name, &v)) n += static_cast<std::size_t>(std::snprintf(line + n, sizeof(line) - n, " %s %lld", s.name, static_cast<long long>(v)));
        if (n >= sizeof(line)) break;
    }
    host_log("player data: the loaded character:%s", line);
}
