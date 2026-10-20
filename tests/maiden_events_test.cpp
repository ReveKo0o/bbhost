// engine/maiden_events on the dump's late maps: each script is checked by
// SHA-256 in and out inside the rewrite; here the result keeps the size,
// differs only in the expected places, and a second pass is refused (the
// input check sees it is no longer the shipped script).
#include "engine/maiden_events.h"
#include "gcn/container.h"
#include "test_app0.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main() {
    int failures = 0, read = 0;
    for (const MaidenEventMap& m : maiden_event_maps()) {
        const std::string path = test_app0_file(("dvdroot_ps4/event/" + std::string(m.map) + ".emevd.dcx").c_str());
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::printf("skip: no %s\n", path.c_str());
            continue;
        }
        ++read;
        const std::vector<std::uint8_t> dcx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string why;
        const std::vector<std::uint8_t> raw = gcn::dcx_decompress(dcx, &why);
        std::vector<std::uint8_t> out = raw;
        if (raw.empty() || !maiden_events_rewrite(m.map, out, &why)) {
            std::printf("FAIL %s: %s\n", m.map, why.c_str());
            ++failures;
            continue;
        }
        std::size_t changed = 0;
        for (std::size_t i = 0; i < raw.size() && i < out.size(); ++i) changed += raw[i] != out[i];
        // 2-3 condition bytes (the count, and the comparison where == turns <)
        // plus a fixed argument's low bytes: a handful, never more.
        if (out.size() != raw.size() || changed == 0 || changed > 12) {
            std::printf("FAIL %s: size %zu -> %zu, %zu bytes changed\n", m.map, raw.size(), out.size(), changed);
            ++failures;
            continue;
        }
        std::vector<std::uint8_t> again = out;
        if (maiden_events_rewrite(m.map, again, &why)) {
            std::printf("FAIL %s: a rewritten script was rewritten again\n", m.map);
            ++failures;
            continue;
        }
        // The archive the game reads round-trips.
        if (gcn::dcx_decompress(gcn::dcx_compress(out), &why) != out) {
            std::printf("FAIL %s: the DCX does not round-trip\n", m.map);
            ++failures;
            continue;
        }
        std::printf("ok %s: %zu bytes changed\n", m.map, changed);
    }
    if (!read) return kTestSkip;
    std::printf(failures ? "maiden_events_test: %d failures\n" : "maiden_events_test: ok\n", failures);
    return failures ? 1 : 0;
}
