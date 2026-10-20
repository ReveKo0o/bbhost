// decomp/insn.cpp over the whole eboot: every function .eh_frame describes
// decoded from its first byte to its last (or to its first jump table),
// which only lands there if every length on the way was right; and every
// function's entry - its
// first whole instructions, 5 bytes or more - relocated, which is what the
// decomp list can now take, against what it could take before (14 bytes,
// nothing rip-relative, no relative branch).
#include "decomp/insn.h"
#include "eboot_kit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main() {
    const char* test = "decomp_insn_eboot";
    const auto& fns = eboot_kit::functions(test);
    std::size_t bad = 0, insns = 0, movable = 0, movable_before = 0, with_tables = 0, small = 0;
    std::size_t refused_loop = 0, refused_into = 0;
    std::uint8_t out[128];
    // BBHOST_INSN_DUMP=file: every instruction's length, for every 97th
    // function and every one not decoded to its end (a check against another
    // disassembler).
    std::FILE* dump = nullptr;
    if (const char* e = std::getenv("BBHOST_INSN_DUMP"); e && *e) dump = std::fopen(e, "w");
    std::size_t index = 0;
    for (const eboot_kit::Function& f : fns) {
        const std::uint8_t* code = eboot_kit::file_at(test, f.bn);
        if (!code) {
            if (++bad <= 10) std::printf("0x%llx: not in the file\n", static_cast<unsigned long long>(f.bn));
            continue;
        }
        // Up to the function's end - or to its first jump table: the eboot
        // keeps those in the code, after the instructions, addressed by a
        // rip-relative lea, and its bytes are no instructions.
        std::size_t at = 0, table = f.size;
        bool ok = true;
        std::string lens;
        while (at < table) {
            const X86Insn x = x86_decode(code + at, table - at);
            if (!x.len) {
                ok = false;
                if (++bad <= 10) {
                    std::printf("0x%llx+0x%zx: not decoded:", static_cast<unsigned long long>(f.bn), at);
                    for (std::size_t k = 0; k < 8 && at + k < f.size; ++k) std::printf(" %02x", code[at + k]);
                    std::printf("\n");
                }
                break;
            }
            if (x.disp_at) {
                std::int32_t d;
                std::memcpy(&d, code + at + x.disp_at, 4);
                const std::int64_t to = static_cast<std::int64_t>(at + x.len) + d;
                if (to > static_cast<std::int64_t>(at + x.len) && to < static_cast<std::int64_t>(table))
                    table = static_cast<std::size_t>(to);
            }
            lens += ' ' + std::to_string(x.len);
            at += x.len;
            ++insns;
        }
        if (table < f.size) ++with_tables;
        if (ok && at != table && ++bad <= 10)
            std::printf("0x%llx: the last instruction runs past 0x%zx\n", static_cast<unsigned long long>(f.bn), table);
        if (dump && (!ok || at != table || index % 97 == 0))
            std::fprintf(dump, "%llx %u%s\n", static_cast<unsigned long long>(f.bn), f.size, lens.c_str());
        ++index;
        if (!ok) continue;
        if (f.size < 5) {
            ++small;  // a ret, a jmp: shorter than the decomp list's jump
            continue;
        }
        // The entry: the fewest whole instructions covering 5 bytes.
        std::size_t n = 0;
        bool plain14 = true;
        std::size_t m = 0;
        while (m < 14 && m < f.size) {
            const X86Insn x = x86_decode(code + m, f.size - m);
            if (x.disp_at || x.rel_at) plain14 = false;
            m += x.len;
            if (!n && m >= 5) n = m;
        }
        if (m >= 14 && plain14) ++movable_before;
        if (x86_relocate(code, n, f.bn, out, f.bn + 0x10000000, sizeof out)) {
            ++movable;
            continue;
        }
        // Why not: a loop/jrcxz among them, or else a branch back into them.
        bool loop = false;
        for (std::size_t k = 0; k < n;) {
            const X86Insn x = x86_decode(code + k, n - k);
            loop = loop || x.kind == X86Insn::Loop;
            k += x.len;
        }
        ++(loop ? refused_loop : refused_into);
    }
    std::printf("decomp_insn_eboot: %zu functions (%zu with jump tables), %zu instructions, %zu not decoded to their end "
                "or first table\n",
                fns.size(), with_tables, insns, bad);
    std::printf("decomp_insn_eboot: entries the decomp list can take: %zu (before: %zu); %zu shorter than 5 bytes; "
                "refused: %zu with a loop/jrcxz, %zu branching back into the entry\n",
                movable, movable_before, small, refused_loop, refused_into);
    return bad ? 1 : 0;
}
