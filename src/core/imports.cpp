#include "core/imports.h"
#include "host/frame_stats.h"
#include "core/thunk.h"
#include "hle/hle.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/nid_table.inc"
#include "core/plt_names.inc"

namespace {

std::unordered_map<std::string, std::string> g_nid_to_name;
std::unordered_map<std::string, void*> g_hle;
std::unordered_set<std::string> g_hle_raw;

void ensure_nid_map() {
    if (!g_nid_to_name.empty()) {
        return;
    }
    for (int i = 0; i < kNidTableCount; ++i) {
        g_nid_to_name.emplace(kNidTable[i].nid, kNidTable[i].name);
    }
}

}  // namespace

void register_hle_fn(const char* name, void* fn) {
    g_hle[name] = fn;
}

void register_hle_fn_raw(const char* name, void* fn) {
    g_hle[name] = fn;
    g_hle_raw.insert(name);
}

std::string lookup_nid_name(std::string_view nid) {
    ensure_nid_map();
    auto it = g_nid_to_name.find(std::string(nid));
    if (it == g_nid_to_name.end()) {
        return {};
    }
    return it->second;
}

std::uint64_t bind_import(const std::string& name, const std::string& nid, std::uint64_t got_va,
                         int plt_index) {
    register_hle();
    // JUMP_SLOT NIDs in this eboot often point into the previous strtab entry.
    // PLT order (kPltNames) is the ground truth for which GOT the game calls.
    std::string resolved = name;
    if (plt_index >= 0 && plt_index < kPltNameCount && kPltNames[plt_index] &&
        kPltNames[plt_index][0] != '\0') {
        resolved = kPltNames[plt_index];
    }
    void* fn = nullptr;
    if (!resolved.empty()) {
        auto it = g_hle.find(resolved);
        if (it != g_hle.end()) {
            fn = it->second;
            if (g_hle_raw.count(resolved)) {
                return reinterpret_cast<std::uint64_t>(fn);
            }
        }
    }
    if (!fn) {
        fn = reinterpret_cast<void*>(hle_make_stub(resolved, nid, got_va));
    }
    return reinterpret_cast<std::uint64_t>(hle_wrap_fn(fn));
}

std::uint64_t bind_glob_dat(const std::string& name, const std::string& nid, std::uint64_t elf_offset,
                           unsigned st_info) {
    constexpr unsigned kSttFunc = 2;
    if ((st_info & 0xf) == kSttFunc) {
        return bind_import(name, nid, elf_offset);
    }
    register_hle();
    if (!name.empty()) {
        auto it = g_hle.find(name);
        if (it != g_hle.end()) {
            return reinterpret_cast<std::uint64_t>(it->second);
        }
    }
    static std::uint64_t anon[32];
    static int anon_n = 0;
    if (elf_offset == 0x53e3be0 || name == "__stack_chk_guard") {
        auto it = g_hle.find("__stack_chk_guard");
        if (it != g_hle.end()) {
            return reinterpret_cast<std::uint64_t>(it->second);
        }
    }
    if (anon_n < 32) {
        return reinterpret_cast<std::uint64_t>(&anon[anon_n++]);
    }
    return bind_import(name, nid, elf_offset);
}

// BBHOST_HLE_COUNT=1: the window's HLE calls by thread group (a name's digits
// dropped, so GXWorker:1-5 count together) and function, the busiest first.
const char* hle_fn_name(void* fn) {
    static const std::unordered_map<void*, const char*> names = [] {
        std::unordered_map<void*, const char*> m;
        for (const auto& [name, f] : g_hle) m[f] = name.c_str();
        return m;
    }();
    const auto it = names.find(fn);
    return it == names.end() ? nullptr : it->second;
}

void hle_call_counts_report() {
    std::unordered_map<void*, const char*> names;
    for (const auto& [name, fn] : g_hle) names[fn] = name.c_str();
    std::map<std::string, std::map<std::string, std::uint64_t>> groups;
    for (const HleCallCount& c : hle_call_counts_take()) {
        std::string g;
        for (char ch : c.thread) {
            if (!(ch >= '0' && ch <= '9')) g += ch;
        }
        const auto it = names.find(c.fn);
        char addr[32];
        std::snprintf(addr, sizeof(addr), "%p", c.fn);
        groups[g][it != names.end() ? it->second : addr] += c.calls;
    }
    for (const auto& [group, fns] : groups) {
        std::vector<std::pair<std::uint64_t, std::string>> v;
        std::uint64_t total = 0;
        for (const auto& [fn, n] : fns) {
            v.push_back({n, fn});
            total += n;
        }
        std::sort(v.rbegin(), v.rend());
        std::string top;
        for (std::size_t i = 0; i < v.size() && i < 12; ++i) top += " " + v[i].second + " " + std::to_string(v[i].first);
        host_log("hle calls: %s %llu:%s", group.c_str(), static_cast<unsigned long long>(total), top.c_str());
    }
    main_wait_sites_report();
}
