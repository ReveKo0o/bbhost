#include "engine/dream_mirror_layout.h"

#include "core/sha256.h"

#include <algorithm>
#include <cstring>

namespace {

// The decompressed m21_00_00_00.msb of the 1.09 dump, and this rewrite of it.
constexpr const char* kInputSha256 = "8e1de0ee223cd5137a93383cfc7bb1d27964bc480c63e35013674717b403ef8f";
constexpr const char* kOutputSha256 = "99d5c7b7f52307344c6e8296b499d5e57969f21c443fcc607cb42cb2c38c8afa";

constexpr std::size_t kFrom = 807;        // ダミーCHR_再キャラメイク, the first DummyEnemy
constexpr std::size_t kTo = 742;          // after ダミーCHR_鍛冶, the workbench dummy
constexpr std::size_t kFirstEnemy = 736;  // the Enemy group's first slot
constexpr std::int32_t kEnemy = 2, kDummyEnemy = 10;
// Entry-relative fields that hold a part's slot.
constexpr std::uint32_t kEventFields[] = {0x48, 0x50, 0x5c};
constexpr std::uint32_t kPartFields[] = {0x128, 0x13c, 0x144, 0x178, 0x180, 0x188};

std::int32_t rd32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int32_t v;
    std::memcpy(&v, b.data() + at, sizeof(v));
    return v;
}
std::int64_t rd64(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::int64_t v;
    std::memcpy(&v, b.data() + at, sizeof(v));
    return v;
}
void wr32(std::vector<std::uint8_t>& b, std::size_t at, std::int32_t v) { std::memcpy(b.data() + at, &v, sizeof(v)); }
void wr64(std::vector<std::uint8_t>& b, std::size_t at, std::int64_t v) { std::memcpy(b.data() + at, &v, sizeof(v)); }

// One of the MSB's four lists (MODEL, EVENT, POINT, PARTS, in file order):
// a header (i32 version, i32 entries + 1, i64 name offset), the entries'
// absolute offsets, then the next list's offset.
struct List {
    std::size_t table = 0;  // where the entry offsets start
    std::vector<std::size_t> entries;
};

bool read_lists(const std::vector<std::uint8_t>& b, List out[4]) {
    std::size_t at = 0x10;
    for (int k = 0; k < 4; ++k) {
        if (at + 16 > b.size()) return false;
        const std::int32_t count = rd32(b, at + 4);
        if (count < 1 || at + 16 + 8 * static_cast<std::size_t>(count) > b.size()) return false;
        out[k].table = at + 16;
        out[k].entries.clear();
        for (std::int32_t i = 0; i + 1 < count; ++i) {
            const std::int64_t e = rd64(b, out[k].table + 8 * static_cast<std::size_t>(i));
            if (e <= 0 || static_cast<std::size_t>(e) >= b.size()) return false;
            out[k].entries.push_back(static_cast<std::size_t>(e));
        }
        const std::int64_t next = rd64(b, out[k].table + 8 * static_cast<std::size_t>(count - 1));
        if (k < 3 && (next <= 0 || static_cast<std::size_t>(next) >= b.size())) return false;
        at = static_cast<std::size_t>(next);
    }
    return true;
}

std::int32_t remap(std::int32_t slot) {
    if (slot >= static_cast<std::int32_t>(kTo) && slot < static_cast<std::int32_t>(kFrom)) return slot + 1;
    if (slot == static_cast<std::int32_t>(kFrom)) return static_cast<std::int32_t>(kTo);
    return slot;
}

}  // namespace

bool dream_mirror_layout(std::vector<std::uint8_t>& msb, std::string* why) {
    const auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (sha256_hex(msb.data(), msb.size()) != kInputSha256) return fail("not the 1.09 m21_00_00_00 layout");
    std::vector<std::uint8_t> b = msb;
    List lists[4];
    if (!read_lists(b, lists)) return fail("the layout's lists do not read");
    List& events = lists[1];
    List& parts = lists[3];
    if (parts.entries.size() <= kFrom + 1) return fail("too few parts");

    // An entry runs to the next entry anywhere in the file (or the end).
    std::vector<std::size_t> starts;
    for (const List& l : lists) starts.insert(starts.end(), l.entries.begin(), l.entries.end());
    std::sort(starts.begin(), starts.end());
    const auto extent = [&](std::size_t entry) {
        const auto it = std::upper_bound(starts.begin(), starts.end(), entry);
        return (it == starts.end() ? b.size() : *it) - entry;
    };

    // 1) The fields naming a part by its slot, in every event and part (the
    //    moved one too), as they will be after the move.
    const auto renumber = [&](const List& l, const std::uint32_t* fields, std::size_t n) {
        for (const std::size_t e : l.entries) {
            const std::size_t size = extent(e);
            for (std::size_t i = 0; i < n; ++i) {
                if (fields[i] + 4 > size) continue;
                const std::int32_t v = rd32(b, e + fields[i]);
                if (v >= 0 && remap(v) != v) wr32(b, e + fields[i], remap(v));
            }
        }
    };
    renumber(events, kEventFields, sizeof(kEventFields) / sizeof(kEventFields[0]));
    renumber(parts, kPartFields, sizeof(kPartFields) / sizeof(kPartFields[0]));

    // 2) Type indices: the Enemies from slot kTo on move down one, the
    //    DummyEnemies after kFrom up one; the part itself becomes an Enemy.
    for (std::size_t i = 0; i < parts.entries.size(); ++i) {
        const std::size_t e = parts.entries[i];
        const std::int32_t type = rd32(b, e + 0x14), index = rd32(b, e + 0x18);
        if (type == kEnemy && i >= kTo) wr32(b, e + 0x18, index + 1);
        if (type == kDummyEnemy && i > kFrom) wr32(b, e + 0x18, index - 1);
    }
    const std::size_t moved = parts.entries[kFrom];
    if (rd32(b, moved + 0x14) != kDummyEnemy) return fail("the stand-in is not a DummyEnemy");
    wr32(b, moved + 0x14, kEnemy);
    wr32(b, moved + 0x18, static_cast<std::int32_t>(kTo - kFirstEnemy));

    // 3) The bytes: the part's entry to slot kTo's place, the entries between
    //    down by its size, the parts table rewritten to the new places (the
    //    file's only absolute reference into this range).
    const std::size_t start = parts.entries[kTo], from = parts.entries[kFrom], end = parts.entries[kFrom + 1];
    if (!(start < from && from < end)) return fail("the parts are not in file order");
    const std::size_t size = end - from;
    std::rotate(b.begin() + static_cast<std::ptrdiff_t>(start), b.begin() + static_cast<std::ptrdiff_t>(from),
                b.begin() + static_cast<std::ptrdiff_t>(end));
    for (std::size_t i = kTo; i < kFrom; ++i)
        wr64(b, parts.table + 8 * (i + 1), static_cast<std::int64_t>(parts.entries[i] + size));
    wr64(b, parts.table + 8 * kTo, static_cast<std::int64_t>(start));

    if (sha256_hex(b.data(), b.size()) != kOutputSha256) return fail("the rewrite is not the known result");
    msb.swap(b);
    return true;
}
