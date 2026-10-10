// Device-local copies of the guest memory draws read as vertex and index data.
//
// Guest direct memory reaches the GPU as imported host memory (gpu.cpp's 1 GiB
// chunks), so every vertex fetch and index read crossed PCIe: in the world at
// a 60 fps target the GPU pulled ~7 GB/s from the host, ~120 MB a frame, with
// its shader units 98% busy and its own memory 8% - waiting on system-memory
// latency. The same meshes are read every frame and almost never change.
//
// Here each dmem chunk gets a device-local mirror, allocated when a draw first
// reads from it, holding guest memory at the same offset as the import, kept
// in 64 KiB pages. A page a draw reads (from a vertex record's extent or an
// index range) is copied into the mirror once and read from there. It stays
// valid while:
//
//   - it takes no CPU write through the guest's view (core/write_watch, as the
//     texture cache does, checked once per recording): a write marks it dirty
//     and the next draw that reads it copies it again;
//   - the command processor writes nothing over it (DMA, WRITE_DATA: on the
//     CPU, through record() in gnm_exec.cpp): the same;
//   - the GPU writes nothing over it (copy and fill dispatches, copy tokens):
//     that write lands later in the command stream than any copy made now
//     could, so such a page is never mirrored again.
//
// A page copied more than kMaxUploads times in kUploadWindowMs holds data
// the game keeps rewriting (rings of dynamic geometry): it, and every draw
// that reads it, stays in host memory from then on. The window is eight
// seconds, not one: a ring the game rewrites every half second to two - the
// vertex data it builds on the CPU (0x2cdd6b9), say - never reached four
// copies in a second, and each pass over it cost a copy and a write-watch
// fault per 4 KiB written (~98% of a soak's ~200k faults). All the state is per
// page, so a draw's check is an array index per page it touches; ranges keyed
// by address grew without bound (ring allocations are new addresses every
// frame) and walking them cost ~1 us a draw.
//
// Copies go into a command buffer of their own that is submitted just before
// the draws' (submit_locked): a barrier at its start keeps them after the
// reads earlier submissions make, one at its end before this submission's
// reads. Copying a dirty range there makes the draws recorded before the write
// read the new contents too - which is also what reading the host memory at
// execution time gave them.
//
// BBHOST_BUFFER_SHADOW=0 binds everything from the imports, as before, and so
// do an integrated GPU and tight memory unless BBHOST_BUFFER_SHADOW=1
// (buffer_shadow_on).

#include "core/write_watch.h"
#include "host/gpu_internal.h"
#include "log.h"

#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <utility>

namespace gpu {

namespace {

constexpr std::uint32_t kMaxUploads = 4;
constexpr std::uint64_t kUploadWindowMs = 8000;
constexpr unsigned kPageShift = 16;  // 64 KiB: validity, write watches and copies go by these
constexpr std::uint64_t kPageBytes = 1ull << kPageShift;
constexpr std::uint32_t kPagesPerChunk = static_cast<std::uint32_t>(kChunkBytes >> kPageShift);

// BBHOST_SHADOW_VERIFY=1: every submission re-hashes a batch of valid pages
// against their contents when copied, and reports (and stops mirroring) any
// that changed without a write this cache saw - a write it cannot track,
// such as a compute shader's store through the GPU page table.
const bool g_verify = [] {
    const char* e = std::getenv("BBHOST_SHADOW_VERIFY");
    return e && e[0] == '1';
}();
std::uint64_t g_verify_cursor = 0, g_verify_checked = 0, g_verify_stale = 0;
// Verify mode: each page's contents when copied, so a stale page's report
// says which bytes changed and to what.
std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> g_verify_copies;

const int g_shadow_env = [] {
    const char* e = std::getenv("BBHOST_BUFFER_SHADOW");
    return !e || !*e ? -1 : e[0] == '0' ? 0 : 1;
}();

// A 64 KiB page of a chunk's mirror: everything is tracked here, so a draw's
// check is an array index per page. Ranges that overlap share their pages, so
// a vertex pool read from a thousand bases is copied once.
struct Page {
    WriteWatch watch;
    WriteWatch alias_watch;           // over the GPU alias, which GX writes through (unarmed when there is none)
    std::uint64_t va = 0, bytes = 0;  // the guest view the watch is armed on (the page within one mapping)
    std::uint64_t alias_va = 0;       // where alias_watch is armed
    std::uint64_t checked = 0;        // the recording (record_serial + 1) it was last checked for writes in
    std::uint64_t window_ms = 0;
    std::uint32_t window_uploads = 0;
    std::uint64_t gpu_written = 0;    // the recording a GPU write over it was recorded in (0: none)
    std::uint64_t hash = 0;           // BBHOST_SHADOW_VERIFY: its contents when copied
    bool valid = false;               // the mirror holds it (or will, once this submission's copies run)
    bool dynamic = false;             // bound from the import from now on (rewritten, unwatchable)
};

struct Mirror {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool failed = false;
    std::vector<Page> pages;
    // A bit per page: checked in the recording `ok_stamp` and valid. Vertex
    // records span up to hundreds of pages (pools), and a draw whose pages
    // are all set costs a few word tests instead of a walk.
    std::vector<std::uint64_t> ok;
    std::uint64_t ok_stamp = 0;
    // A bit per page for what sends a draw to host memory, so that draw
    // takes a few word tests too instead of a walk over the pages' records:
    // bound from host memory for good (Page::dynamic), and written by the
    // GPU in the recording `ok_stamp` (Page::gpu_written; cleared with ok).
    std::vector<std::uint64_t> dyn, gpu;
    // Sparse: a 64 KiB device page is bound the first time the page is
    // copied, from 64 MiB blocks; the binds queue up until the submission.
    bool sparse = false;
    std::uint32_t mem_type_bits = 0;
    std::vector<VkDeviceMemory> blocks;
    std::uint32_t block_pages_used = 0;  // in blocks.back()
    std::vector<std::uint64_t> resident;  // a bit per page
    std::vector<VkSparseMemoryBind> pending;
    std::uint64_t resident_pages = 0;
};
constexpr std::uint32_t kBlockPages = 1024;  // 64 MiB blocks
VkSemaphore g_bind_sem[kSlots] = {};

bool all_ok(const std::vector<std::uint64_t>& ok, std::uint32_t p0, std::uint32_t p1) {
    for (std::uint32_t w = p0 >> 6; w <= p1 >> 6; ++w) {
        std::uint64_t mask = ~0ull;
        if (w == p0 >> 6) mask &= ~0ull << (p0 & 63);
        if (w == p1 >> 6) mask &= ~0ull >> (63 - (p1 & 63));
        if ((ok[w] & mask) != mask) return false;
    }
    return true;
}

bool any_set(const std::vector<std::uint64_t>& bits, std::uint32_t p0, std::uint32_t p1) {
    for (std::uint32_t w = p0 >> 6; w <= p1 >> 6; ++w) {
        std::uint64_t mask = ~0ull;
        if (w == p0 >> 6) mask &= ~0ull << (p0 & 63);
        if (w == p1 >> 6) mask &= ~0ull >> (63 - (p1 & 63));
        if (bits[w] & mask) return true;
    }
    return false;
}

Mirror g_mirrors[gcn::kDmemChunks];
// The video memory the mirrors may hold: g.local_budget less kImageRoom, the
// room kept for images and targets - what the longest sessions' took in the
// world at 1080p (5.8 GiB) and some to spare. A full
// mirror is a whole chunk (sparse ones run only on NVIDIA's driver): on an
// 8 GB AMD card, about 7 GiB of budget, four of them left the world's targets
// no memory and the world drew black. BBHOST_SHADOW_VRAM_MB sets the share
// instead; once the mirrors are given back (shadow_give_back_locked) it is 0.
constexpr std::uint64_t kImageRoom = 6656ull << 20;
std::uint64_t g_mirror_cap = 0;    // shadow_budget_locked
std::uint64_t g_mirror_bytes = 0;  // video memory the mirrors hold: whole mirrors and sparse blocks
bool g_given_back = false;
VkCommandBuffer g_upload_cmd[kSlots] = {};
int g_upload_slot = -1;  // the slot whose upload buffer is recording, or -1
std::size_t g_last_map = 0;  // g.maps index of the last hit

std::atomic<std::uint64_t> g_hits{0}, g_uploads{0}, g_upload_bytes{0}, g_dynamic{0}, g_imports{0}, g_gpu_written{0};
std::uint64_t g_page_checks = 0, g_invalid_cp = 0, g_invalid_gpu = 0;
std::uint64_t g_span_hist[6] = {};  // calls by pages spanned: 1, 2-4, 5-16, 17-64, 65-256, more

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Guest VA -> (chunk, offset) through the snapshot of direct-memory mappings,
// with the mapping's bounds; false outside one mapping and one chunk.
bool phys_of(std::uint64_t va, std::uint64_t bytes, std::uint32_t& chunk, std::uint64_t& off, std::uint64_t* map_lo = nullptr,
             std::uint64_t* map_hi = nullptr) {
    // The last hit first, with no walk: draws come in runs, and nearly all
    // of them land in the one big direct mapping.
    const std::size_t n = g.maps.size();
    std::size_t i = g_last_map;
    if (i >= n || va < g.maps[i].va || va >= g.maps[i].va + g.maps[i].len) {
        for (i = 0; i < n; ++i) {
            if (va >= g.maps[i].va && va < g.maps[i].va + g.maps[i].len) break;
        }
        if (i == n) return false;
        g_last_map = i;
    }
    const GuestMapInfo& mi = g.maps[i];
    if (g_import_audit) {
        import_audit(mi, 2);
        audit_range(va, bytes, kUseMirror);
    }
    if (!mi.dmem || va + bytes > mi.va + mi.len) return false;
    const std::uint64_t phys = static_cast<std::uint64_t>(mi.phys) + (va - mi.va);
    chunk = static_cast<std::uint32_t>(phys >> gcn::kDmemChunkShift);
    off = phys & (kChunkBytes - 1);
    if (map_lo) *map_lo = mi.va;
    if (map_hi) *map_hi = mi.va + mi.len;
    return chunk < gcn::kDmemChunks && dmem_import(chunk, off, bytes) != nullptr;
}

Mirror* mirror_for(std::uint32_t chunk) {
    Mirror& m = g_mirrors[chunk];
    if (m.buffer || m.failed) return m.failed ? nullptr : &m;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = kChunkBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkMemoryRequirements req{};
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (g.has_sparse) {
        // A sparse buffer the size of the chunk, no memory yet: pages are
        // bound as they are copied. The sparse block must divide our page.
        bci.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        if (vkCreateBuffer(g.device, &bci, nullptr, &m.buffer) == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(g.device, m.buffer, &req);
            if (req.alignment == 0 || (kPageBytes % req.alignment) != 0 ||
                find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == UINT32_MAX) {
                host_log("buffer shadow: sparse block %llu does not fit a %llu-byte page; full mirrors instead",
                         static_cast<unsigned long long>(req.alignment), static_cast<unsigned long long>(kPageBytes));
                vkDestroyBuffer(g.device, m.buffer, nullptr);
                m.buffer = VK_NULL_HANDLE;
            } else {
                m.sparse = true;
                m.mem_type_bits = req.memoryTypeBits;
                m.resident.assign(kPagesPerChunk / 64, 0);
                m.pages.resize(kPagesPerChunk);
                m.ok.assign(kPagesPerChunk / 64, 0);
                m.dyn.assign(kPagesPerChunk / 64, 0);
                m.gpu.assign(kPagesPerChunk / 64, 0);
                host_log("buffer shadow: sparse mirror for chunk %u (pages bound as copied)", chunk);
                return &m;
            }
        } else {
            m.buffer = VK_NULL_HANDLE;
        }
        bci.flags = 0;
    }
    if (g_mirror_bytes + kChunkBytes > g_mirror_cap) {
        m.failed = true;
        static bool said = false;
        if (!said && !g_given_back) {
            said = true;
            host_log("buffer shadow: no mirror for chunk %u: another %llu MiB would pass the mirrors' share of video memory (%llu MiB, "
                     "%llu held); a chunk without one is read from host memory",
                     chunk, static_cast<unsigned long long>(kChunkBytes >> 20), static_cast<unsigned long long>(g_mirror_cap >> 20),
                     static_cast<unsigned long long>(g_mirror_bytes >> 20));
        }
        return nullptr;
    }
    if (vkCreateBuffer(g.device, &bci, nullptr, &m.buffer) != VK_SUCCESS) {
        m.buffer = VK_NULL_HANDLE;
    } else {
        vkGetBufferMemoryRequirements(g.device, m.buffer, &req);
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.device, &mai, nullptr, &m.memory) != VK_SUCCESS) {
            vkDestroyBuffer(g.device, m.buffer, nullptr);
            m.buffer = VK_NULL_HANDLE;
        } else {
            vkBindBufferMemory(g.device, m.buffer, m.memory, 0);
            g_mirror_bytes += req.size;
        }
    }
    if (!m.buffer) {
        m.failed = true;
        host_log("buffer shadow: no device-local mirror for chunk %u; its vertex and index data stay in host memory", chunk);
        return nullptr;
    }
    m.pages.resize(kPagesPerChunk);
    m.ok.assign(kPagesPerChunk / 64, 0);
    m.dyn.assign(kPagesPerChunk / 64, 0);
    m.gpu.assign(kPagesPerChunk / 64, 0);
    host_log("buffer shadow: device-local mirror for chunk %u (%llu MiB)", chunk,
             static_cast<unsigned long long>(kChunkBytes >> 20));
    return &m;
}

VkCommandBuffer upload_cmd_locked() {
    if (g_upload_slot == g.slot) return g_upload_cmd[g.slot];
    VkCommandBuffer& cmd = g_upload_cmd[g.slot];
    if (!cmd) {
        // A pool of its own: these are recorded on the command processor
        // while the draw recorder (recorder.cpp) records g.cmd_, and a pool's
        // command buffers are recorded one thread at a time.
        static VkCommandPool pool = [] {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = g.family;
            VkCommandPool p = VK_NULL_HANDLE;
            if (vkCreateCommandPool(g.device, &pci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
            return p;
        }();
        if (!pool) return VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g.device, &cai, &cmd) != VK_SUCCESS) {
            cmd = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    // Earlier submissions' reads of the mirror finish before these writes.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    g_upload_slot = g.slot;
    return cmd;
}

// Written since its copy, through the CPU view or the GPU alias.
bool written(Page& pg) {
    bool w = write_watch_dirty(pg.va, pg.bytes, pg.watch) != 0;
    if (pg.alias_va) w |= write_watch_dirty(pg.alias_va, pg.bytes, pg.alias_watch) != 0;
    return w;
}

void go_dynamic(Mirror& m, std::uint32_t p, std::uint64_t va, const char* why) {
    Page& pg = m.pages[p];
    pg.dynamic = true;
    pg.valid = false;
    m.dyn[p >> 6] |= 1ull << (p & 63);
    if (g_dynamic.fetch_add(1, std::memory_order_relaxed) < 8) {
        host_log("buffer shadow: the page at 0x%llx %s; bound from host memory from now on", static_cast<unsigned long long>(va), why);
    }
}

// The per-recording bits start empty in each recording: whichever of a
// draw's check or a GPU write comes first in it clears them.
void begin_recording(Mirror& m, std::uint64_t stamp) {
    if (m.ok_stamp == stamp) return;
    std::fill(m.ok.begin(), m.ok.end(), 0);
    std::fill(m.gpu.begin(), m.gpu.end(), 0);
    m.ok_stamp = stamp;
}

}  // namespace

// Off with BBHOST_BUFFER_SHADOW=0 and, unless it is 1, on an integrated GPU -
// its device-local memory is the RAM the imports already are, so a mirror
// saves no bus - and with tight memory (gpu.cpp, memory_tight): a mirror is a
// gigabyte a chunk, and on a Steam Deck (1 GiB of VRAM, 8 GiB of GTT, which
// every submission's buffers have to fit together) the second one made the
// submissions fail.
bool buffer_shadow_on() { return g_shadow_env >= 0 ? g_shadow_env == 1 : !g.integrated && !g.tight; }

void shadow_locate_locked(std::uint64_t va, std::uint64_t bytes, Located& loc) {
    if (!buffer_shadow_on() || !bytes || !loc.buffer) return;
    std::uint32_t chunk = 0;
    std::uint64_t off = 0, map_lo = 0, map_hi = 0;
    if (!phys_of(va, bytes, chunk, off, &map_lo, &map_hi)) return;
    Mirror* m = mirror_for(chunk);
    if (!m) return;
    const std::uint64_t stamp = g.record_serial + 1;
    const std::uint32_t p0 = static_cast<std::uint32_t>(off >> kPageShift);
    const std::uint32_t p1 = static_cast<std::uint32_t>((off + bytes - 1) >> kPageShift);
    {
        const std::uint32_t span = p1 - p0 + 1;
        ++g_span_hist[span == 1 ? 0 : span <= 4 ? 1 : span <= 16 ? 2 : span <= 64 ? 3 : span <= 256 ? 4 : 5];
    }
    begin_recording(*m, stamp);
    if (all_ok(m->ok, p0, p1)) {
        bump(g_hits);
        loc.buffer = m->buffer;
        loc.offset = off;
        loc.avail = bytes;
        return;
    }
    // A page bound from host memory for good, or one a GPU write in this
    // recording left uncopied (a copy now would run before that write): the
    // walk below would stop at it, having looked at every page before it.
    if (any_set(m->dyn, p0, p1) || any_set(m->gpu, p0, p1)) {
        bump(g_imports);
        return;
    }
    // The pages this call will copy, armed a run at a time: one protection of
    // the guest view and one of its GPU alias per run of consecutive pages,
    // instead of two per 64 KiB page - each is an mprotect, and the new
    // buffers of a fight's first frame armed hundreds of pages (~9% of the
    // command processor there). The walk stops where the loop below would;
    // a run that cannot be armed whole is left to the loop, a page at a time.
    thread_local std::vector<std::uint8_t> prearmed;  // per page of [p0, p1]: 1 view armed, 2 alias armed
    prearmed.assign(p1 - p0 + 1, 0);
    {
        const auto arm_run = [&](std::uint32_t a, std::uint32_t b) {  // pages [a, b)
            if (b - a < 2) return;  // one page: the loop's own arming is the same work
            const std::uint64_t first_va = va - (off - (static_cast<std::uint64_t>(a) << kPageShift));
            const std::uint64_t lo = std::max(first_va, map_lo);
            const std::uint64_t hi = std::min(first_va + (static_cast<std::uint64_t>(b - a) << kPageShift), map_hi);
            WriteWatch w, aw;
            if (!hle_kernel_write_watch(lo, hi - lo, w)) return;
            const std::uint64_t alias = hle_kernel_write_watch_alias(lo, hi - lo, aw);
            for (std::uint32_t p = a; p < b; ++p) {
                Page& pg = m->pages[p];
                pg.watch = w;
                prearmed[p - p0] = 1;
                if (alias) {
                    const std::uint64_t page_va = va - (off - (static_cast<std::uint64_t>(p) << kPageShift));
                    pg.alias_watch = aw;
                    pg.alias_va = alias + (std::max(page_va, map_lo) - lo);
                    prearmed[p - p0] |= 2;
                }
            }
        };
        const std::uint64_t t = now_ms();
        std::uint32_t run = p0, p = p0;
        for (; p <= p1; ++p) {
            Page& pg = m->pages[p];
            // The loop's checks in its order, with only its write check's
            // effects (once per recording, as the loop makes it).
            if (pg.dynamic || (!pg.valid && pg.gpu_written == stamp)) break;
            if (pg.valid && pg.checked != stamp) {
                pg.checked = stamp;
                ++g_page_checks;
                if (written(pg)) pg.valid = false;
            }
            if (pg.valid) {  // nothing to copy: the run ends before it
                arm_run(run, p);
                run = p + 1;
                continue;
            }
            const std::uint32_t uploads = t - pg.window_ms >= kUploadWindowMs ? 0 : pg.window_uploads;
            if (uploads + 1 > kMaxUploads) break;  // the loop sends it to host memory and stops there
        }
        arm_run(run, p);
    }
    // Pages to copy, as runs, from the import that holds them all.
    const Chunk* source = dmem_import(chunk, static_cast<std::uint64_t>(p0) << kPageShift, static_cast<std::uint64_t>(p1 - p0 + 1) << kPageShift);
    if (!source) {
        bump(g_imports);
        return;
    }
    VkBufferCopy regions[16];
    std::uint32_t n_regions = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    auto flush = [&] {
        if (n_regions) {
            vkCmdCopyBuffer(cmd, source->buffer, m->buffer, n_regions, regions);
            
            // Post-copy memory barrier for the NVIDIA driver (Access Violation prevention)
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = m->buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,
                0, nullptr,
                1, &barrier,
                0, nullptr
            );
        }
        n_regions = 0;
    };
    for (std::uint32_t p = p0; p <= p1; ++p) {
        Page& pg = m->pages[p];
        const std::uint64_t page_va = va - (off - (static_cast<std::uint64_t>(p) << kPageShift));
        if (pg.dynamic) {
            flush();
            bump(g_imports);
            return;
        }
        // Written by the GPU in this recording: a copy now would run before
        // that write. In a later recording the copy is ordered after it.
        if (!pg.valid && pg.gpu_written == stamp) {
            flush();
            bump(g_imports);
            return;
        }
        if (pg.valid) {
            if (pg.checked != stamp) {
                pg.checked = stamp;
                ++g_page_checks;
                if (written(pg)) pg.valid = false;
            }
            if (pg.valid) {
                m->ok[p >> 6] |= 1ull << (p & 63);
                continue;
            }
        }
        // Copy it. A page copied more than kMaxUploads times in the window
        // keeps being rewritten: host memory serves it better.
        const std::uint64_t t = now_ms();
        if (t - pg.window_ms >= kUploadWindowMs) {
            pg.window_ms = t;
            pg.window_uploads = 0;
        }
        if (++pg.window_uploads > kMaxUploads) {
            go_dynamic(*m, p, page_va, "keeps being rewritten");
            flush();
            bump(g_imports);
            return;
        }
        // Watched from before the copy reads it: a write after that marks it
        // again rather than going unseen. Only the part in this mapping.
        const std::uint64_t lo = std::max(page_va, map_lo), hi = std::min(page_va + kPageBytes, map_hi);
        if (!(prearmed[p - p0] & 1) && !hle_kernel_write_watch(lo, hi - lo, pg.watch)) {
            go_dynamic(*m, p, page_va, "cannot be write-watched");
            flush();
            bump(g_imports);
            return;
        }
        if (!(prearmed[p - p0] & 2)) pg.alias_va = hle_kernel_write_watch_alias(lo, hi - lo, pg.alias_watch);
        if (!cmd && !(cmd = upload_cmd_locked())) {
            pg.watch = WriteWatch{};
            pg.alias_watch = WriteWatch{};
            pg.alias_va = 0;
            return;
        }
        pg.va = lo;
        pg.bytes = hi - lo;
        pg.valid = true;
        pg.checked = stamp;
        if (g_verify) {
            const auto* src = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(lo));
            pg.hash = fnv1a(src, hi - lo);
            g_verify_copies[(std::uint64_t{chunk} << 32) | p].assign(src, src + (hi - lo));
        }
        m->ok[p >> 6] |= 1ull << (p & 63);
        const VkDeviceSize at = static_cast<VkDeviceSize>(p) << kPageShift;
        if (m->sparse && !(m->resident[p >> 6] & (1ull << (p & 63)))) {
            // Bind a device page for it; the bind runs on the queue before
            // this recording's copies (shadow_bind_sparse_locked).
            if (m->blocks.empty() || m->block_pages_used == kBlockPages) {
                VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                bai.allocationSize = static_cast<VkDeviceSize>(kBlockPages) * kPageBytes;
                bai.memoryTypeIndex = find_memory_type(m->mem_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                VkDeviceMemory mem = VK_NULL_HANDLE;
                if (g_mirror_bytes + bai.allocationSize > g_mirror_cap || vkAllocateMemory(g.device, &bai, nullptr, &mem) != VK_SUCCESS) {
                    go_dynamic(*m, p, page_va, g_mirror_bytes + bai.allocationSize > g_mirror_cap
                                                   ? "has no room in the mirrors' share of video memory"
                                                   : "has no device memory for its mirror page");
                    flush();
                    bump(g_imports);
                    return;
                }
                g_mirror_bytes += bai.allocationSize;
                m->blocks.push_back(mem);
                m->block_pages_used = 0;
            }
            VkSparseMemoryBind b{};
            b.resourceOffset = at;
            b.size = kPageBytes;
            b.memory = m->blocks.back();
            b.memoryOffset = static_cast<VkDeviceSize>(m->block_pages_used++) * kPageBytes;
            m->pending.push_back(b);
            m->resident[p >> 6] |= 1ull << (p & 63);
            ++m->resident_pages;
        }
        // The source is that import, which starts at the chunk's byte lo.
        const VkDeviceSize src = at - source->lo;
        if (n_regions && regions[n_regions - 1].srcOffset + regions[n_regions - 1].size == src) {
            regions[n_regions - 1].size += kPageBytes;
        } else {
            if (n_regions == 16) flush();
            regions[n_regions++] = {src, at, kPageBytes};
        }
        bump(g_uploads);
        bump(g_upload_bytes, kPageBytes);
    }
    flush();
    bump(g_hits);
    loc.buffer = m->buffer;
    loc.offset = off;
    loc.avail = bytes;
}

// For a diagnostic: what the mirror would do with this range right now, and -
// the question that matters - whether a page it is still serving has been
// written since the recording's one check of it. A valid page is checked once
// per recording; a guest write after that check is not seen again until the
// next one, so `valid + checked-this-recording + written` is a page whose
// mirror the GPU will read stale. Pure: `write_watch_dirty` only memoises a
// clean answer, and a written watch stays written until it is armed again.
void shadow_probe_locked(std::uint64_t va, std::uint64_t bytes, char* out, std::size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!buffer_shadow_on() || !bytes) {
        std::snprintf(out, n, "shadow off");
        return;
    }
    std::uint32_t chunk = 0;
    std::uint64_t off = 0;
    if (!phys_of(va, bytes, chunk, off, nullptr, nullptr)) {
        std::snprintf(out, n, "not in a mirrored chunk");
        return;
    }
    Mirror* m = mirror_for(chunk);
    if (!m) {
        std::snprintf(out, n, "no mirror");
        return;
    }
    const std::uint64_t stamp = g.record_serial + 1;
    const std::uint32_t p0 = static_cast<std::uint32_t>(off >> kPageShift);
    const std::uint32_t p1 = static_cast<std::uint32_t>((off + bytes - 1) >> kPageShift);
    std::size_t at = 0;
    std::uint32_t stale = 0;
    for (std::uint32_t p = p0; p <= p1 && at + 48 < n; ++p) {
        Page& pg = m->pages[p];
        const bool w = pg.valid && written(pg);
        if (pg.valid && pg.checked == stamp && w) ++stale;
        at += static_cast<std::size_t>(std::snprintf(out + at, n - at, "%s%u:%s%s%s%s", at ? " " : "", p,
                                                     pg.dynamic ? "host" : pg.valid ? "mirror" : "uncopied",
                                                     pg.valid && pg.checked == stamp ? "+checked" : "",
                                                     w ? "+WRITTEN" : "",
                                                     pg.gpu_written == stamp ? "+gpuwrite" : ""));
    }
    if (stale && at + 32 < n) std::snprintf(out + at, n - at, " <= %u STALE", stale);
}

void shadow_written_locked(std::uint64_t va, std::uint64_t bytes, bool by_gpu) {
    if (!buffer_shadow_on() || !bytes) return;
    std::uint32_t chunk = 0;
    std::uint64_t off = 0, map_lo = 0, map_hi = 0;
    if (!phys_of(va, 1, chunk, off, &map_lo, &map_hi)) return;
    Mirror& m = g_mirrors[chunk];
    if (m.pages.empty()) return;
    const std::uint64_t stamp = g.record_serial + 1;
    begin_recording(m, stamp);
    const std::uint64_t end = std::min<std::uint64_t>(off + std::min(bytes, map_hi - va), kChunkBytes);
    std::uint64_t gpu_pages = 0;
    for (std::uint64_t p = off >> kPageShift; p <= (end - 1) >> kPageShift; ++p) {
        Page& pg = m.pages[p];
        m.ok[p >> 6] &= ~(1ull << (p & 63));
        if (pg.valid) {
            pg.valid = false;
            ++(by_gpu ? g_invalid_gpu : g_invalid_cp);
        }
        if (by_gpu) {
            pg.gpu_written = stamp;
            m.gpu[p >> 6] |= 1ull << (p & 63);
            ++gpu_pages;
        }
    }
    if (gpu_pages) g_gpu_written.fetch_add(gpu_pages, std::memory_order_relaxed);
}

namespace {
// Pages valid for a while (checked in an earlier recording, so their copy has
// run) whose memory no longer hashes as it did when copied.
void verify_batch_locked() {
    const std::uint64_t stamp = g.record_serial + 1;
    for (std::uint32_t chunk = 0; chunk < gcn::kDmemChunks; ++chunk) {
        Mirror& m = g_mirrors[chunk];
        if (m.pages.empty()) continue;
        for (int n = 0; n < 256; ++n) {
            const std::uint32_t p = static_cast<std::uint32_t>(g_verify_cursor++ % kPagesPerChunk);
            Page& pg = m.pages[p];
            if (!pg.valid || pg.checked == stamp || !pg.va) continue;
            const std::uint64_t page_va = pg.va & ~(kPageBytes - 1);
            if (!hle_kernel_va_mapped(pg.va, pg.bytes)) continue;
            ++g_verify_checked;
            if (fnv1a(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(pg.va)), pg.bytes) == pg.hash) continue;
            if (written(pg)) continue;  // a write the next check will see anyway
            if (g_verify_stale++ < 32) {
                host_log("buffer shadow: VERIFY: page 0x%llx changed with no write this cache saw (chunk %u)",
                         static_cast<unsigned long long>(page_va), chunk);
                hle_kernel_mappings_of(pg.va);  // logs each address the page is mapped at
                auto it = g_verify_copies.find((std::uint64_t{chunk} << 32) | p);
                if (it != g_verify_copies.end() && it->second.size() == pg.bytes) {
                    const auto* now = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(pg.va));
                    const auto* was = reinterpret_cast<const std::uint32_t*>(it->second.data());
                    std::uint32_t changed = 0, first = ~0u, last = 0;
                    std::string sample;
                    for (std::uint32_t k = 0; k < pg.bytes / 4; ++k) {
                        if (now[k] == was[k]) continue;
                        ++changed;
                        first = std::min(first, k * 4);
                        last = k * 4;
                        if (changed <= 6) {
                            char b[96];
                            float fw, fn;
                            std::memcpy(&fw, &was[k], 4);
                            std::memcpy(&fn, &now[k], 4);
                            std::snprintf(b, sizeof(b), " +0x%x: %08x->%08x (%g->%g)", k * 4, was[k], now[k], fw, fn);
                            sample += b;
                        }
                    }
                    host_log("buffer shadow: VERIFY:   %u dwords changed in [+0x%x, +0x%x]:%s", changed, first, last, sample.c_str());
                }
                char d[512];
                write_watch_describe(pg.va, pg.bytes, pg.watch, d, sizeof(d));
                host_log("buffer shadow: VERIFY:   CPU view watch: %s", d);
                if (pg.alias_va) {
                    write_watch_describe(pg.alias_va, pg.bytes, pg.alias_watch, d, sizeof(d));
                    host_log("buffer shadow: VERIFY:   alias 0x%llx watch: %s", static_cast<unsigned long long>(pg.alias_va), d);
                }
                if (std::FILE* f = std::fopen("/proc/self/maps", "r")) {
                    char line[256];
                    while (std::fgets(line, sizeof(line), f)) {
                        unsigned long long a = 0, b = 0;
                        if (std::sscanf(line, "%llx-%llx", &a, &b) != 2) continue;
                        const bool cpu = a < pg.va + pg.bytes && b > pg.va;
                        const bool alias = pg.alias_va && a < pg.alias_va + pg.bytes && b > pg.alias_va;
                        if (cpu || alias) host_log("buffer shadow: VERIFY:   maps %s", line);
                    }
                    std::fclose(f);
                }
            }
            go_dynamic(m, p, page_va, "changed without a tracked write");
        }
    }
}
}  // namespace

// The copies this recording made, ended; submit_locked runs them first.
VkCommandBuffer shadow_end_uploads_locked() {
    if (g_verify) verify_batch_locked();
    if (g_upload_slot != g.slot) return VK_NULL_HANDLE;
    const VkCommandBuffer cmd = g_upload_cmd[g.slot];
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);
    g_upload_slot = -1;
    return cmd;
}

VkSemaphore shadow_bind_sparse_locked() {
    VkSparseBufferMemoryBindInfo infos[gcn::kDmemChunks];
    std::uint32_t n = 0;
    for (std::uint32_t c = 0; c < gcn::kDmemChunks; ++c) {
        Mirror& m = g_mirrors[c];
        if (!m.sparse || m.pending.empty()) continue;
        infos[n].buffer = m.buffer;
        infos[n].bindCount = static_cast<std::uint32_t>(m.pending.size());
        infos[n].pBinds = m.pending.data();
        ++n;
    }
    if (!n) return VK_NULL_HANDLE;
    VkSemaphore& sem = g_bind_sem[g.slot];
    if (!sem) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(g.device, &sci, nullptr, &sem) != VK_SUCCESS) sem = VK_NULL_HANDLE;
    }
    VkBindSparseInfo bsi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bsi.bufferBindCount = n;
    bsi.pBufferBinds = infos;
    bsi.signalSemaphoreCount = sem ? 1 : 0;
    bsi.pSignalSemaphores = sem ? &sem : nullptr;
    VkResult r;
    {
        QueueGuard queue;
        r = vkQueueBindSparse(g.queue, 1, &bsi, VK_NULL_HANDLE);
    }
    static int logs = 0;
    if (r != VK_SUCCESS && logs++ < 5) host_log("buffer shadow: vkQueueBindSparse failed (%d)", r);
    for (std::uint32_t c = 0; c < gcn::kDmemChunks; ++c) g_mirrors[c].pending.clear();
    return r == VK_SUCCESS ? sem : VK_NULL_HANDLE;
}

void shadow_budget_locked() {
    const char* e = std::getenv("BBHOST_SHADOW_VRAM_MB");
    g_mirror_cap = e && *e ? static_cast<std::uint64_t>(std::strtoull(e, nullptr, 10)) << 20
                           : g.local_budget > kImageRoom ? g.local_budget - kImageRoom : 0;
    if (!buffer_shadow_on()) return;
    if (!g.has_sparse && g_mirror_cap < kChunkBytes) {
        host_log("buffer shadow: no mirrors - one is a whole %llu MiB chunk here (no sparse binding), and the mirrors' share of video "
                 "memory is %llu MiB (%s); vertex and index data are read from host memory",
                 static_cast<unsigned long long>(kChunkBytes >> 20), static_cast<unsigned long long>(g_mirror_cap >> 20),
                 e && *e ? "BBHOST_SHADOW_VRAM_MB" : "what is free above the 6656 MiB kept for images and targets");
    } else {
        host_log("buffer shadow: its mirrors may hold %llu MiB of video memory (%s)", static_cast<unsigned long long>(g_mirror_cap >> 20),
                 e && *e ? "BBHOST_SHADOW_VRAM_MB" : "what is free above the 6656 MiB kept for images and targets");
    }
}

void shadow_give_back_locked(Gpu::Slot& slot) {
    std::uint32_t mirrors = 0;
    for (Mirror& m : g_mirrors) {
        if (!m.buffer) continue;
        slot.garbage.push_back(DevBuffer{m.memory, m.buffer});
        for (VkDeviceMemory b : m.blocks) slot.garbage.push_back(DevBuffer{b, VK_NULL_HANDLE});
        m = Mirror{};
        m.failed = true;
        ++mirrors;
    }
    const std::uint64_t held = g_mirror_bytes;
    g_mirror_bytes = 0;
    g_mirror_cap = 0;
    if (!g_given_back) {
        g_given_back = true;
        host_log("buffer shadow: video memory ran short - %u mirror(s), %llu MiB, given back to the images; vertex and index data are "
                 "read from host memory from now on", mirrors, static_cast<unsigned long long>(held >> 20));
    }
}

std::string shadow_report() {
    std::uint64_t resident = 0;
    for (std::uint32_t c = 0; c < gcn::kDmemChunks; ++c) resident += g_mirrors[c].resident_pages;
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "buffer shadow: %llu binds from device memory, %llu page copies (%.1f MiB), %llu from host memory; %llu pages "
                  "dynamic, %llu GPU writes over them; %llu page checks; invalidations %llu by the CP and %llu by the GPU",
                  static_cast<unsigned long long>(g_hits.load()), static_cast<unsigned long long>(g_uploads.load()),
                  static_cast<double>(g_upload_bytes.load()) / (1 << 20), static_cast<unsigned long long>(g_imports.load()),
                  static_cast<unsigned long long>(g_dynamic.load()), static_cast<unsigned long long>(g_gpu_written.load()),
                  static_cast<unsigned long long>(g_page_checks), static_cast<unsigned long long>(g_invalid_cp),
                  static_cast<unsigned long long>(g_invalid_gpu));
    if (g_verify) {
        char v[96];
        std::snprintf(v, sizeof(v), "; verified %llu, stale %llu", static_cast<unsigned long long>(g_verify_checked),
                      static_cast<unsigned long long>(g_verify_stale));
        std::strncat(buf, v, sizeof(buf) - std::strlen(buf) - 1);
    }
    char h[160];
    std::snprintf(h, sizeof(h), "; spans 1:%llu 2-4:%llu 5-16:%llu 17-64:%llu 65-256:%llu more:%llu",
                  static_cast<unsigned long long>(g_span_hist[0]), static_cast<unsigned long long>(g_span_hist[1]),
                  static_cast<unsigned long long>(g_span_hist[2]), static_cast<unsigned long long>(g_span_hist[3]),
                  static_cast<unsigned long long>(g_span_hist[4]), static_cast<unsigned long long>(g_span_hist[5]));
    char rr[160];
    std::snprintf(rr, sizeof(rr), "; mirror pages resident %llu (%llu MiB); mirrors hold %llu MiB of video memory, share %llu MiB%s",
                  static_cast<unsigned long long>(resident), static_cast<unsigned long long>((resident * kPageBytes) >> 20),
                  static_cast<unsigned long long>(g_mirror_bytes >> 20), static_cast<unsigned long long>(g_mirror_cap >> 20),
                  g_given_back ? " (given back: video memory ran short)" : "");
    return std::string(buf) + h + rr;
}

}  // namespace gpu

std::string host_gpu_shadow_report() {
    std::lock_guard<GpuMutex> lock(gpu::g.mu);
    return gpu::shadow_report();
}

void host_gpu_shadow_cp_write(std::uint64_t va, std::size_t bytes) {
    using namespace gpu;
    std::lock_guard<GpuMutex> lock(g.mu);
    shadow_written_locked(va, bytes, false);
}
