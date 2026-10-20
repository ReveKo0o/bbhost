#include "engine/maiden_events.h"

#include "core/sha256.h"

#include <cstring>

namespace {

// The EMEVD (64-bit) as the maps have it: a header of 16 int64 at +0x10 -
// [0] events, [1] the event table, [3] the instruction table, [13] the
// argument blob - events of 0x30 bytes (id, instruction count, offset into
// the instruction table, ...), instructions of 0x20 (bank, id, argument
// length, offset into the blob, ...).
struct Ins {
    std::int32_t bank, id;
    std::size_t arg_at, arg_len;  // absolute offset in the file
};

std::int64_t rd64(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int64_t v;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}
std::int32_t rd32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

// Every instruction of event `id` that has arguments - one without has the
// offset 0xffffffff (m34's event 0 holds four) and nothing to rewrite; empty
// when there is none (or the file is short).
std::vector<Ins> event_instructions(const std::vector<std::uint8_t>& b, std::int64_t id) {
    std::vector<Ins> out;
    if (b.size() < 0x90) return out;
    const std::int64_t events = rd64(b, 0x10), event_at = rd64(b, 0x18), ins_at = rd64(b, 0x28), args_at = rd64(b, 0x78);
    for (std::int64_t e = 0; e < events; ++e) {
        const std::size_t ev = static_cast<std::size_t>(event_at + e * 0x30);
        if (ev + 0x30 > b.size() || rd64(b, ev) != id) continue;
        const std::int64_t count = rd64(b, ev + 8), first = rd64(b, ev + 16);
        for (std::int64_t k = 0; k < count; ++k) {
            const std::size_t in = static_cast<std::size_t>(ins_at + first + k * 0x20);
            if (in + 0x20 > b.size()) return {};
            Ins i;
            i.bank = rd32(b, in);
            i.id = rd32(b, in + 4);
            i.arg_len = static_cast<std::size_t>(rd64(b, in + 8));
            if (i.arg_len == 0) continue;
            i.arg_at = static_cast<std::size_t>(args_at + rd64(b, in + 16));
            if (i.arg_at + i.arg_len > b.size()) return {};
            out.push_back(i);
        }
    }
    return out;
}

struct Fix {  // an argument of event 0's start of `event` in `slot`
    std::int32_t event, slot, index, was, now;
};
struct MapRule {
    MaidenEventMap map;
    std::int32_t ring, stop;  // the maiden's events
    std::vector<Fix> fixes;
};

const std::vector<MapRule>& rules() {
    static const std::vector<MapRule> r = {
        {{"m26_00_00_00", "95d168643c0ca1e96d77577be2723dc15ad6af8588e729ca6ab76cd609bc9331",
          "4d84a87b673117ea2158ba7ccaa50ba22796bb2303b5d333121ea1dee8c1ec8b"},
         12604710, 12604720, {}},
        {{"m33_00_00_00", "9375a331f1727e6d1f89f233773af1ffdeaa1eba97f97e1122dc6fc83ee78ff4",
          "533f8409794420e62e1bcaea0f2297c85801e7cc04286a05d8e31a17d81e875c"},
         13304710, 13304720, {}},
        {{"m34_00_00_00", "61cbe85daf9097619094e053edcab1233432c86619b96198845d5469e917268c",
          "499cd490e24214f81f3fa1d50b459acd945a802574b09a7c9bfe74b883a24311"},
         13404710, 13404720, {{13404710, 5, 0, 3400701, 3400791}}},
        {{"m35_00_00_00", "4989dfbe383c30e65847c1abfc5d8e09adea0b6caff11320f864069566e1e628",
          "e38558b6f8ebd80b07cdf16e4759e901c91922c687c9de59396e8d94773479af"},
         13504710, 13504720, {{13504700, 5, 2, 12604712, 13504712}}},
        {{"m36_00_00_00", "9ea8f27a0e5b93884b528bbab79bc7da52a890aef07ae886f777df2450583639",
          "e257328d7eafad901c39575615e9b6d5e01251c3438a55ce49265757b1da820e"},
         13604710, 13604720, {}},
    };
    return r;
}

// 3[29]'s argument: [condition group s8][client type u8][comparison u8][count u8];
// comparison 0 is ==, 3 <, 4 >=; client type 1 is an invader.
constexpr std::uint8_t kRingWas[4] = {1, 1, 0, 0}, kRingNow[4] = {1, 1, 3, 2};
constexpr std::uint8_t kStopWas[4] = {0xff, 1, 4, 1}, kStopNow[4] = {0xff, 1, 4, 2};

}  // namespace

const std::vector<MaidenEventMap>& maiden_event_maps() {
    static const std::vector<MaidenEventMap> maps = [] {
        std::vector<MaidenEventMap> v;
        for (const MapRule& r : rules()) v.push_back(r.map);
        return v;
    }();
    return maps;
}

bool maiden_events_rewrite(const std::string& map, std::vector<std::uint8_t>& emevd, std::string* why) {
    const MapRule* rule = nullptr;
    for (const MapRule& r : rules()) {
        if (map == r.map.map) rule = &r;
    }
    auto fail = [&](const std::string& w) {
        if (why) *why = map + ": " + w;
        return false;
    };
    if (!rule) return fail("not a map this rewrites");
    if (sha256_hex(emevd.data(), emevd.size()) != rule->map.sha_in) return fail("not the 1.09 event script");
    std::vector<std::uint8_t> out = emevd;
    int swapped = 0;
    for (const std::int32_t id : {rule->ring, rule->stop}) {
        for (const Ins& i : event_instructions(out, id)) {
            if (i.bank != 3 || i.id != 29 || i.arg_len < 4) continue;
            std::uint8_t* a = out.data() + i.arg_at;
            if (std::memcmp(a, kRingWas, 4) == 0) {
                std::memcpy(a, kRingNow, 4);
                ++swapped;
            } else if (std::memcmp(a, kStopWas, 4) == 0) {
                std::memcpy(a, kStopNow, 4);
                ++swapped;
            }
        }
    }
    int fixed = 0;
    for (const Ins& i : event_instructions(out, 0)) {
        if (i.bank != 2000 || i.id != 0 || i.arg_len < 8) continue;
        const std::int32_t slot = rd32(out, i.arg_at), event = rd32(out, i.arg_at + 4);
        for (const Fix& f : rule->fixes) {
            const std::size_t at = i.arg_at + 8 + 4 * static_cast<std::size_t>(f.index);
            if (event != f.event || slot != f.slot || at + 4 > i.arg_at + i.arg_len || rd32(out, at) != f.was) continue;
            std::memcpy(out.data() + at, &f.now, 4);
            ++fixed;
        }
    }
    if (sha256_hex(out.data(), out.size()) != rule->map.sha_out) {
        return fail("the rewrite came out unexpected (" + std::to_string(swapped) + " conditions, " +
                    std::to_string(fixed) + " fixes)");
    }
    emevd = std::move(out);
    return true;
}
