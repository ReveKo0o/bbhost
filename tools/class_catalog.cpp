// The engine's classes by the names they register, from the 1.09 eboot alone
// (docs/decomp.md, "Adding one": name from evidence). Dantelion's classes
// register a runtime class at start - DLRF::DLRuntimeClass::RegisterClass
// with the class's name - and keep it in a slot that the class's
// GetRuntimeClass reads; the class's vtables point at that GetRuntimeClass.
// So, for every registration:
//   - the name (it registers a UTF-16 copy too, listed where it differs),
//   - the slot, and the function that registers it,
//   - the functions that read the slot (GetRuntimeClass, and the runtime
//     class's own methods),
//   - the vtables holding one of those functions - the class's own, its
//     slots listed - and the functions that store each (its constructors and
//     destructors).
// A class that does not override GetRuntimeClass shares its parent's, so
// its vtable is listed under the parent; the slot count tells them apart.
//
// The registrations sit in static initializers, which - like many small
// methods - have no .eh_frame entry, so the whole executable segment is
// read: the functions .eh_frame describes, and the code between them up to
// the next known start (a function's, a vtable slot's or a call's target).
//
//   build/class_catalog > symbols/1.09-classes.txt      (BBHOST_EBOOT=...)
//   build/class_catalog --names > symbols/1.09-class-names.tsv
// --names prints names in symbols/1.09-names.tsv's columns (address, name,
// confidence, source, evidence): each class's GetRuntimeClass, and
// Class::vfN for a slot only that class's vtables hold.
#include "decomp/insn.h"
#include "eboot_kit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using u64 = std::uint64_t;
using ull = unsigned long long;
constexpr u64 kRegisterClass = 0x24b7b10;  // DLRF::DLRuntimeClass::RegisterClass
const char* const kTool = "class_catalog";

struct Registration {
    std::string name, name2;  // the name, and its UTF-16 copy
    u64 slot = 0, by = 0;
    std::set<u64> readers;      // functions reading the slot, the registering one aside
    std::set<u64> own_vtables;  // vtables the registering function stores (the runtime class's own)
};

struct Region {
    u64 lo, hi;  // instructions from lo, at most to hi
};

std::vector<u64> g_starts;                          // every function start known, sorted
std::unordered_map<u64, std::vector<u64>> g_refs;   // rip-relative target -> instructions referring to it

u64 function_of(u64 a) {
    auto it = std::upper_bound(g_starts.begin(), g_starts.end(), a);
    return it == g_starts.begin() ? 0 : *(it - 1);
}

std::string text_at(u64 a) {
    const std::uint8_t* p = eboot_kit::file_at(kTool, a);
    if (!p) return {};
    std::string s;
    for (std::size_t i = 0; i < 256 && p[i]; ++i) s += static_cast<char>(p[i]);
    return s;
}

// The same name in UTF-16 (the second one a class registers), as ASCII.
std::string wide_at(u64 a) {
    const std::uint8_t* p = eboot_kit::file_at(kTool, a);
    if (!p) return {};
    std::string s;
    for (std::size_t i = 0; i < 512 && (p[i] || p[i + 1]); i += 2) s += p[i + 1] ? '?' : static_cast<char>(p[i]);
    return s;
}

// A region's instructions, until one does not decode or its first jump table.
template <class Visit>
void each_insn(const Region& r, Visit visit) {
    const std::uint8_t* code = eboot_kit::file_at(kTool, r.lo);
    if (!code) return;
    std::size_t at = 0, end = r.hi - r.lo;
    while (at < end) {
        const X86Insn x = x86_decode(code + at, end - at);
        if (!x.len) return;
        const u64 pc = r.lo + at;
        u64 target = 0;
        if (x.disp_at || x.rel_size == 4) {
            std::int32_t d;
            std::memcpy(&d, code + at + (x.disp_at ? x.disp_at : x.rel_at), 4);
            target = pc + x.len + static_cast<u64>(static_cast<std::int64_t>(d));
            if (x.disp_at && target > pc && target < r.hi) end = static_cast<std::size_t>(target - r.lo);  // a jump table
        }
        visit(code + at, x, pc, target);
        at += x.len;
    }
}

// REX.W op modrm, with the modrm's register (REX.R folded in); -1 if not that shape.
int rexw_reg(const std::uint8_t* p, const X86Insn& x, std::uint8_t op, int mod, int rm, int disp8 = -1) {
    if (x.len < 3 || (p[0] & 0xf8) != 0x48 || p[1] != op) return -1;
    const std::uint8_t m = p[2];
    if ((m >> 6) != mod || (m & 7) != rm) return -1;
    if (disp8 >= 0 && (x.len != 4 || p[3] != disp8)) return -1;
    return ((p[0] & 4) ? 8 : 0) | ((m >> 3) & 7);
}

}  // namespace

int main(int argc, char** argv) {
    const bool names = argc > 1 && std::strcmp(argv[1], "--names") == 0;
    eboot_kit::load(kTool);
    const auto& fns = eboot_kit::functions(kTool);
    const eboot_kit::Range text = eboot_kit::text(kTool);
    const auto in_text = [&](u64 a) { return a >= text.lo && a < text.hi; };

    // The regions: each function .eh_frame describes, and each gap between
    // them (and after the last).
    std::vector<Region> regions;
    u64 cursor = text.lo;
    for (const auto& f : fns) {
        if (f.bn > cursor) regions.push_back({cursor, f.bn});
        regions.push_back({f.bn, f.bn + f.size});
        cursor = std::max<u64>(cursor, f.bn + f.size);
    }
    if (cursor < text.hi) regions.push_back({cursor, text.hi});

    // Function starts: .eh_frame's, every relocated pointer into the code
    // (vtable slots and function tables), every call's target.
    std::set<u64> starts;
    for (const auto& f : fns) starts.insert(f.bn);
    for (const auto& r : eboot_kit::relocated())
        if (in_text(r.target)) starts.insert(r.target);
    for (const Region& r : regions)
        each_insn(r, [&](const std::uint8_t*, const X86Insn& x, u64, u64 target) {
            if (x.kind == X86Insn::Call && in_text(target)) starts.insert(target);
        });
    g_starts.assign(starts.begin(), starts.end());

    // A gap's code split at the starts inside it, so nothing runs on from
    // one function into the next; every rip-relative reference noted.
    std::vector<Region> pieces;
    for (const Region& r : regions) {
        u64 lo = r.lo;
        for (auto it = std::upper_bound(g_starts.begin(), g_starts.end(), r.lo); it != g_starts.end() && *it < r.hi; ++it) {
            pieces.push_back({lo, *it});
            lo = *it;
        }
        pieces.push_back({lo, r.hi});
    }
    std::vector<const Region*> registering;
    for (const Region& r : pieces) {
        bool calls = false;
        each_insn(r, [&](const std::uint8_t*, const X86Insn& x, u64 pc, u64 target) {
            if (x.disp_at) g_refs[target].push_back(pc);
            if (x.kind == X86Insn::Call && target == kRegisterClass) calls = true;
        });
        if (calls) registering.push_back(&r);
    }

    // Vtables: runs of relocated pointers into the code, 8 bytes apart.
    std::vector<eboot_kit::Relocated> rel = eboot_kit::relocated();
    std::sort(rel.begin(), rel.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    std::map<u64, std::vector<u64>> vtables;               // address point -> slots' targets
    std::unordered_map<u64, std::vector<u64>> in_vtables;  // function -> vtables holding it
    for (std::size_t i = 0; i < rel.size();) {
        if (!in_text(rel[i].target)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        std::vector<u64> slots;
        while (j < rel.size() && in_text(rel[j].target) && rel[j].slot == rel[i].slot + 8 * (j - i)) slots.push_back(rel[j++].target);
        for (u64 t : slots) in_vtables[t].push_back(rel[i].slot);
        vtables[rel[i].slot] = std::move(slots);
        i = j;
    }

    // The registrations, in each registering function's order.
    std::vector<Registration> regs;
    for (const Region* r : registering) {
        u64 reg[16] = {};
        u64 last_slot = 0, name = 0, name2 = 0;
        std::set<u64> stored;  // vtables written into the runtime class object
        each_insn(*r, [&](const std::uint8_t* p, const X86Insn& x, u64, u64 target) {
            int k;
            if (x.disp_at && (k = rexw_reg(p, x, 0x8d, 0, 5)) >= 0) {
                reg[k] = target;  // lea r64, [rip+target]
                if (vtables.count(target)) stored.insert(target);
            } else if ((k = rexw_reg(p, x, 0x89, 1, 7, 0x40)) >= 0) {
                name = reg[k];  // mov [rdi+0x40], r64
            } else if ((k = rexw_reg(p, x, 0x89, 1, 7, 0x48)) >= 0) {
                name2 = reg[k];  // mov [rdi+0x48], r64
            } else if (x.disp_at && (rexw_reg(p, x, 0x89, 0, 5) >= 0 || rexw_reg(p, x, 0x8b, 0, 5) >= 0)) {
                last_slot = target;  // mov [rip+slot], r64 / mov r64, [rip+slot]
            } else if (x.kind == X86Insn::Call && target == kRegisterClass) {
                Registration g;
                g.name = text_at(name);
                g.name2 = wide_at(name2);
                g.slot = last_slot;
                g.by = function_of(r->lo);
                g.own_vtables = stored;
                if (!g.name.empty() && g.slot) regs.push_back(std::move(g));
                name = name2 = 0;
                stored.clear();
            }
        });
    }

    // What reads each slot, and the vtables holding those readers.
    struct Named {
        const Registration* g;
        u64 vt;
    };
    std::vector<Named> named;
    std::size_t with_vtable = 0;
    std::set<u64> named_vtables;
    std::sort(regs.begin(), regs.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    for (Registration& g : regs) {
        for (u64 pc : g_refs[g.slot])
            if (u64 fn = function_of(pc); fn != g.by) g.readers.insert(fn);
        std::set<u64> vts;
        for (u64 fn : g.readers)
            for (u64 vt : in_vtables[fn])
                if (!g.own_vtables.count(vt)) vts.insert(vt);
        for (u64 vt : vts) named.push_back({&g, vt});
        if (names) continue;
        std::printf("class %s", g.name.c_str());
        if (!g.name2.empty() && g.name2 != g.name) std::printf("  (%s)", g.name2.c_str());
        std::printf("\n  runtime class slot 0x%llx, registered by 0x%llx\n", static_cast<ull>(g.slot), static_cast<ull>(g.by));
        std::printf("  reads the slot:");
        for (u64 fn : g.readers) std::printf(" 0x%llx", static_cast<ull>(fn));
        std::printf("\n");
        if (!vts.empty()) ++with_vtable;
        for (u64 vt : vts) {
            named_vtables.insert(vt);
            const auto& slots = vtables[vt];
            std::printf("  vtable 0x%llx, %zu slots:", static_cast<ull>(vt), slots.size());
            for (std::size_t k = 0; k < slots.size(); ++k)
                std::printf(" %zu:0x%llx%s", k, static_cast<ull>(slots[k]), g.readers.count(slots[k]) ? "(GetRuntimeClass)" : "");
            std::printf("\n    stored by (constructors, destructors):");
            std::set<u64> by;
            for (u64 pc : g_refs[vt]) by.insert(function_of(pc));
            for (u64 fn : by) std::printf(" 0x%llx", static_cast<ull>(fn));
            std::printf("\n");
        }
    }
    if (names) {
        // A slot only one class's vtables hold is that class's own method.
        std::unordered_map<u64, std::set<const Registration*>> holders;
        for (const Named& n : named)
            for (u64 fn : vtables[n.vt]) holders[fn].insert(n.g);
        std::set<u64> done;
        for (const Named& n : named) {
            const auto& slots = vtables[n.vt];
            for (std::size_t k = 0; k < slots.size(); ++k) {
                const u64 fn = slots[k];
                if (holders[fn].size() != 1 || !done.insert(fn).second) continue;
                const bool get = n.g->readers.count(fn) != 0;
                std::printf("0x%llx\t%s::%s\t%s\tclass_catalog\tslot %zu of vtable 0x%llx (runtime class slot 0x%llx)\n",
                            static_cast<ull>(fn), n.g->name.c_str(), get ? "GetRuntimeClass" : ("vf" + std::to_string(k)).c_str(),
                            get ? "high" : "medium", k, static_cast<ull>(n.vt), static_cast<ull>(n.g->slot));
            }
        }
        std::fprintf(stderr, "class_catalog: %zu names\n", done.size());
        return 0;
    }
    std::size_t multi = 0;
    for (const auto& [vt, slots] : vtables) multi += slots.size() >= 2;
    std::printf("\n%zu registrations by %zu functions; %zu with a vtable found; %zu vtables named, of %zu runs of two or more "
                "code pointers\n",
                regs.size(), registering.size(), with_vtable, named_vtables.size(), multi);
    std::fprintf(stderr, "class_catalog: %zu classes, %zu with vtables, %zu vtables named (of %zu runs of 2+); %zu function starts\n",
                 regs.size(), with_vtable, named_vtables.size(), multi, g_starts.size());
    return 0;
}
