// The plugin SDK (include/bbhost) as one translation unit: every engine
// layout's static_asserts, the generated param rows', and a few run-time
// checks of the symbol table plugins resolve names through.
#include "bbhost/engine/engine.hpp"
#include "bbhost/params.hpp"
#include "bbhost/sdk.hpp"

#include <cstdio>
#include <cstring>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    };
    const bb::NamedSymbol* w = bb::find_symbol("WORLD_CHR_MAN_SINGLETON_PTR");
    check(w && w->addr.bn() == 0x593e878, "WorldChrMan's slot is the one engine/world_chr.cpp reads");
    check(!bb::find_symbol("NO_SUCH_SYMBOL"), "an unknown name is not found");
    check(std::strcmp(bb::params::ItemLotParam::name, "ItemLotParam") == 0, "a table tag names its table");
    // 0x184: the stride of the 1.09 NpcParam.param rows and the game's own
    // definition.
    static_assert(sizeof(bb::params::NpcParam::Row) == 0x184, "NPC_PARAM_ST, as the 1.09 table lays it out");
    bb::Rng a(bb::seed_of("seed")), b(bb::seed_of("seed"));
    check(a.next() == b.next(), "one seed, one sequence");
    if (failures) return 1;
    std::printf("sdk_headers_test: %zu named symbols, all checks passed\n", sizeof(bb::ALL_SYMBOLS) / sizeof(bb::ALL_SYMBOLS[0]));
    return 0;
}
