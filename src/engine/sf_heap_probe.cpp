#include "guest_abi.h"
#include "engine/sf_heap_probe.h"

#include "core/elf.h"
#include "engine/addr.h"
#include "engine/graphics_patch.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace {

// Scaleform 4's MemoryHeapPT in the eboot.
//
// AllocAutoHeap (0x23389c0, the global heap's vtable slot 14) finds the heap
// that owns an address - a 0x5fc0 page header at the page's start or its
// last 16 bytes, indexing the page-descriptor table at 0x5994470, else the
// segment tree - and allocates from that heap's engine (heap+0xa8) through
// 0x233c870. Blocks up to 0x200 bytes come from 4 KiB pages the page source
// (0x233e3a0) takes from the root SysAlloc; larger ones grow the engine
// directly (0x233c660: engine, size, alignment, &retry, info), from the
// heap's own SysAlloc. Engine fields in qwords: [0] the SysAlloc table, [1]
// the heap, [0x46] footprint, [0x47] bytes in use, [0x48] limit, [0x49] the
// limit handler. The grow asks the handler when a request passes the limit
// but allocates either way.
//
// Both SysAllocs the game installs (0x2d296f0 per heap, 0x23c5f30 at
// 0x5a9b628 for the root) allocate from a Dantelion heap at (sysalloc+8)+0x28
// whose free byte count is at +0x18 (object_allocator_mb, 0x2485a60).
constexpr std::uint64_t kGrow = 0x233c660;
constexpr std::uint8_t kGrowPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                          0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};
constexpr std::uint64_t kAllocAuto = 0x23389c0;
constexpr std::uint8_t kAllocAutoPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                               0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};
constexpr std::uint64_t kHeapSysAlloc = 0x2d296f0;
constexpr std::uint64_t kRootSysAlloc = 0x5a9b628;
constexpr std::uint64_t kPageTable = 0x5994470;
constexpr std::uint64_t kPageManager = 0x5940360;

std::uint64_t g_slide = 0;
bool g_on = false;

std::uint64_t va(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }
std::uint64_t bn(std::uint64_t va) { return va - g_slide + kPreferredGuestSlide; }

template <typename T>
T rd(std::uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof(v));
    return v;
}

std::uint64_t dl_free_of(std::uint64_t sysalloc) {
    const std::uint64_t dl = rd<std::uint64_t>(sysalloc + 8);
    return dl ? rd<std::uint64_t>(dl + 0x28 + 0x18) : 0;
}

std::uint64_t root_free() { return dl_free_of(va(kRootSysAlloc)); }

// The SysAlloc the page source takes pages and descriptor chunks from:
// (*manager)+0x20, the manager being *0x5940360.
std::uint64_t page_sysalloc() {
    const std::uint64_t mgr = rd<std::uint64_t>(va(kPageManager));
    return mgr ? rd<std::uint64_t>(rd<std::uint64_t>(mgr) + 0x20) : 0;
}

// The SysAlloc an engine grows from: its table's entry for the heap's index.
std::uint64_t sysalloc_of(std::uint64_t engine) {
    const std::uint64_t table = rd<std::uint64_t>(engine);
    const std::int32_t idx = rd<std::int32_t>(rd<std::uint64_t>(engine + 8) + 0xb8);
    return rd<std::uint64_t>(table + 8 * static_cast<std::uint64_t>(idx + 2));
}

GUEST_ABI std::int64_t grow_hook(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t engine = saved[5], size = saved[4], align = saved[3];
    const std::uint64_t footprint = rd<std::uint64_t>(engine + 0x46 * 8), limit = rd<std::uint64_t>(engine + 0x48 * 8);
    const std::uint64_t sys = sysalloc_of(engine);
    const std::uint64_t sys_fn = bn(rd<std::uint64_t>(rd<std::uint64_t>(sys) + 0x58));
    const std::uint64_t own_free = sys_fn == kHeapSysAlloc ? dl_free_of(sys) : 0;
    const std::uint64_t psys = page_sysalloc();
    const std::uint64_t pages_free = psys ? dl_free_of(psys) : 0;
    static std::mutex mu;
    static std::map<std::uint64_t, int> seen;
    static std::uint64_t last_bucket = ~0ull;
    static int logs = 0;
    std::lock_guard<std::mutex> lk(mu);
    static bool tables = false;
    if (!tables) {
        // SprjMemory's heap sizes: 0x4b36d40[mode][16], the heap named by
        // 0x5758cc0[16] (wide strings), mode 0x552895c then 0x5528958.
        tables = true;
        const std::int32_t m1 = rd<std::int32_t>(va(0x552895c)), m0 = rd<std::int32_t>(va(0x5528958));
        for (int i = 0; i < 16; ++i) {
            char name[40] = {};
            const std::uint64_t w = rd<std::uint64_t>(va(0x5758cc0) + 8 * i);
            for (int k = 0; w && k < 39; ++k) {
                const std::uint16_t c = rd<std::uint16_t>(w + 2 * k);
                if (!c) break;
                name[k] = static_cast<char>(c);
            }
            host_log("sf probe:   heap size table %2d %-24s mode %d: %llx, mode %d: %llx", i, name, m1,
                     m1 >= 0 && m1 <= 8 ? static_cast<unsigned long long>(rd<std::uint64_t>(va(0x4b36d40) + m1 * 128 + i * 8)) : 0ull,
                     m0, m0 >= 0 && m0 <= 8 ? static_cast<unsigned long long>(rd<std::uint64_t>(va(0x4b36d40) + m0 * 128 + i * 8)) : 0ull);
        }
    }
    if (psys && pages_free >> 18 != last_bucket) {
        if (last_bucket == ~0ull) {
            const std::uint64_t dl = rd<std::uint64_t>(psys + 8);
            for (int i = 0; i < 0x80 / 8; i += 4) {
                host_log("sf probe:   page source's Dantelion heap %llx +0x%02x %016llx %016llx %016llx %016llx",
                         static_cast<unsigned long long>(dl), i * 8, static_cast<unsigned long long>(rd<std::uint64_t>(dl + i * 8)),
                         static_cast<unsigned long long>(rd<std::uint64_t>(dl + i * 8 + 8)),
                         static_cast<unsigned long long>(rd<std::uint64_t>(dl + i * 8 + 16)),
                         static_cast<unsigned long long>(rd<std::uint64_t>(dl + i * 8 + 24)));
            }
        }
        last_bucket = pages_free >> 18;
        host_log("sf probe: page source has %llu bytes free (root %llu)", static_cast<unsigned long long>(pages_free),
                 static_cast<unsigned long long>(root_free()));
    }
    const bool over = limit && footprint + size > limit;
    const bool low = (own_free && own_free < 512 * 1024) || pages_free < 256 * 1024;
    if ((over || low || seen[engine]++ < 3) && logs++ < 600) {
        host_log("sf probe: grow engine %llx (heap %llx, SysAlloc %llx: %llu free) %llu bytes align %llu, footprint %llu, "
                 "limit %llu%s",
                 static_cast<unsigned long long>(engine), static_cast<unsigned long long>(rd<std::uint64_t>(engine + 8)),
                 static_cast<unsigned long long>(sys_fn), static_cast<unsigned long long>(own_free),
                 static_cast<unsigned long long>(size), static_cast<unsigned long long>(align),
                 static_cast<unsigned long long>(footprint), static_cast<unsigned long long>(limit),
                 over ? " - over the limit" : "");
    }
    return 0;
}

struct LastAlloc {
    std::uint64_t heap = 0, addr = 0, size = 0, ret = 0;
};
thread_local LastAlloc t_last;

GUEST_ABI std::int64_t alloc_auto_hook(std::uint64_t, const std::uint64_t* saved) {
    t_last = LastAlloc{saved[5], saved[4], saved[3], 0};
    return 0;
}

// AllocAutoHeap's lookup by page header; 0 when the address has none (the
// segment-tree fallback is not repeated here).
std::uint64_t heap_of(std::uint64_t addr) {
    for (const std::uint64_t at : {addr & ~0xfffull, addr | 0xff0ull}) {
        const std::uint64_t h = rd<std::uint64_t>(at);
        if ((h & 0xffff) != 0x5fc0) continue;
        const std::uint64_t slot = (h >> 0x1c) & 0x7f0;
        const std::uint64_t table = rd<std::uint64_t>(va(kPageTable) + slot);
        const std::uint64_t desc = table + (((h >> 0x27) & rd<std::uint64_t>(va(kPageTable) + slot + 8)) << 5);
        if (addr - rd<std::uint64_t>(desc + 0x18) <= 0xfff) return rd<std::uint64_t>(desc + 0x10);
    }
    return 0;
}

}  // namespace

void sf_heap_probe_install(ElfImage* image) {
    const char* e = std::getenv("BBHOST_SF_PROBE");
    if (!e || *e != '1') return;
    g_slide = image->mem.slide;
    const bool grow = engine_prologue_hook(image, va(kGrow), kGrowPrologue, sizeof(kGrowPrologue), reinterpret_cast<void*>(&grow_hook));
    const bool alloc =
        engine_prologue_hook(image, va(kAllocAuto), kAllocAutoPrologue, sizeof(kAllocAutoPrologue), reinterpret_cast<void*>(&alloc_auto_hook));
    g_on = grow || alloc;
    host_log("sf probe: grow path %s, AllocAutoHeap %s", grow ? "hooked" : "NOT hooked", alloc ? "hooked" : "NOT hooked");
}

void sf_heap_probe_crash_report() {
    if (!g_on || !t_last.heap) return;
    const LastAlloc a = t_last;
    host_log("sf probe: this thread's last AllocAutoHeap: %llu bytes near %llx; page source %llu free",
             static_cast<unsigned long long>(a.size), static_cast<unsigned long long>(a.addr),
             static_cast<unsigned long long>(root_free()));
    // The page source (0x233e3a0) over the manager at *0x5940360: [3] is the
    // free page-descriptor list (empty when it points at [2]), [4] the count
    // of descriptor chunks (at most 0x80), and (*[0])+0x20 the SysAlloc pages
    // and chunks come from.
    const std::uint64_t mgr = rd<std::uint64_t>(va(kPageManager));
    const std::uint64_t page_sys = rd<std::uint64_t>(rd<std::uint64_t>(mgr) + 0x20);
    const std::uint64_t page_dl = rd<std::uint64_t>(page_sys + 8);
    host_log("sf probe:   page manager %llx: free descriptors %s, %u descriptor chunks; its SysAlloc %llx (fn %llx) over "
             "Dantelion heap %llx: bitmap %llx, %llu free, %llu blocks",
             static_cast<unsigned long long>(mgr), rd<std::uint64_t>(mgr + 0x18) == mgr + 0x10 ? "none" : "some",
             rd<std::uint32_t>(mgr + 0x20), static_cast<unsigned long long>(bn(page_sys)),
             static_cast<unsigned long long>(bn(rd<std::uint64_t>(rd<std::uint64_t>(page_sys) + 0x58))),
             static_cast<unsigned long long>(page_dl), static_cast<unsigned long long>(rd<std::uint64_t>(page_dl + 0x28)),
             static_cast<unsigned long long>(rd<std::uint64_t>(page_dl + 0x40)),
             static_cast<unsigned long long>(rd<std::uint64_t>(page_dl + 0x48)));
    const std::uint64_t heap = heap_of(a.addr);
    if (!heap) {
        host_log("sf probe:   %llx has no page header (a large block or not a Scaleform address)",
                 static_cast<unsigned long long>(a.addr));
        return;
    }
    const std::uint64_t engine = rd<std::uint64_t>(heap + 0xa8);
    const std::uint64_t sys = sysalloc_of(engine);
    host_log("sf probe:   heap %llx engine %llx: footprint %llu, in use %llu, limit %llu, handler %llx, SysAlloc %llx "
             "(%llu free)",
             static_cast<unsigned long long>(heap), static_cast<unsigned long long>(engine),
             static_cast<unsigned long long>(rd<std::uint64_t>(engine + 0x46 * 8)),
             static_cast<unsigned long long>(rd<std::uint64_t>(engine + 0x47 * 8)),
             static_cast<unsigned long long>(rd<std::uint64_t>(engine + 0x48 * 8)),
             static_cast<unsigned long long>(rd<std::uint64_t>(engine + 0x49 * 8)),
             static_cast<unsigned long long>(bn(rd<std::uint64_t>(rd<std::uint64_t>(sys) + 0x58))),
             static_cast<unsigned long long>(dl_free_of(sys)));
    const auto* h = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(heap));
    for (int i = 0; i < 0xc0 / 8; i += 4) {
        host_log("sf probe:   heap +0x%02x %016llx %016llx %016llx %016llx", i * 8, static_cast<unsigned long long>(h[i]),
                 static_cast<unsigned long long>(h[i + 1]), static_cast<unsigned long long>(h[i + 2]),
                 static_cast<unsigned long long>(h[i + 3]));
    }
}
