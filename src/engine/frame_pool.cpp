#include "engine/frame_pool.h"

#include "core/elf.h"
#include "core/memory.h"
#include "core/thunk.h"
#include "engine/addr.h"
#include "guest_abi.h"
#include "hle/modules.h"
#include "log.h"

#include <atomic>
#include <cstdlib>
#include <sched.h>
#include <cstring>

namespace {

using ull = unsigned long long;

// Guest VAs at the preferred slide, as Binary Ninja shows them.
constexpr std::uint64_t kBorrow = 0x1467f60;      // the function this replaces
constexpr std::uint64_t kPoolOf = 0x1457350;      // () -> the pool, or 0
constexpr std::uint64_t kPrepare = 0x1449890;     // (ctx->0x118, arg2)
constexpr std::uint64_t kAcquire = 0x1456920;     // (pool) -> an entry, or 0 when none is free
constexpr std::uint64_t kRelease = 0x1456990;     // (pool, entry): finds it in the pool and marks it free
constexpr std::uint64_t kKindOf = 0x256a7c0;      // (entry->8) -> a kind, the third argument below
constexpr std::uint64_t kCanUse = 0x2565150;      // (target, 3, kind) -> 0 when it may be used
constexpr std::uint64_t kBegin = 0x2569c30;       // (obj, obj + 0xb730, target, 0, 3, &out) -> 0 on success
constexpr std::uint64_t kHandOver = 0x1449860;    // (ctx->0x118, out)
constexpr std::uint64_t kEnd = 0x2569eb0;         // (obj, obj + 0xb730, target, 0)
// The prologue the replacement writes over: push rbp; mov rbp, rsp; push r15,
// r14, r13, r12, rbx; sub rsp, 0x18 - twelve bytes, and the patch needs twelve.
constexpr std::uint8_t kPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};

std::uint64_t g_slide = 0;
bool g_installed = false;

std::atomic<std::uint64_t> g_borrows{0}, g_clobbered{0}, g_error_released{0}, g_spins{0};
std::atomic<int> g_logged{0};

std::uint64_t rd64(std::uint64_t va) {
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), 8);
    return v;
}
void wr64(std::uint64_t va, std::uint64_t v) {
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(va)), &v, 8);
}
void* guest_fn(std::uint64_t bn_va) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(g_slide + (bn_va - kPreferredGuestSlide)));
}
template <typename... A>
std::int64_t call(std::uint64_t bn_va, A... a) {
    return hle_call_guest<std::int64_t>(guest_fn(bn_va), static_cast<std::int64_t>(a)...);
}

// The pool's entry goes back whichever way this call leaves.
void give_back(std::uint64_t pool, std::uint64_t ctx, std::uint64_t entry) {
    if (rd64(ctx + 0x120) != entry) {
        // Another thread on this context overwrote it. The game's own release
        // would have taken that value, which is 0 while the other thread is
        // still spinning, and freed nothing at all.
        g_clobbered.fetch_add(1, std::memory_order_relaxed);
        if (g_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            host_log("pool: context 0x%llx held entry 0x%llx and now reads 0x%llx - another thread on the same context",
                     static_cast<ull>(ctx), static_cast<ull>(entry), static_cast<ull>(rd64(ctx + 0x120)));
        }
    }
    call(kRelease, pool, entry);
    wr64(ctx + 0x120, 0);
}

GUEST_ABI std::int64_t frame_pool_borrow(std::int64_t ctx_a, std::int64_t arg2) {
    const std::uint64_t ctx = static_cast<std::uint64_t>(ctx_a);
    const std::uint64_t pool = static_cast<std::uint64_t>(call(kPoolOf));
    if (!pool) return 1;
    const std::uint64_t holder = rd64(ctx + 0x118);
    if (!holder) return 1;
    const std::uint64_t target = rd64(holder + 0x40);
    if (!target) return 1;
    call(kPrepare, holder, arg2);

    // The entry is this call's, so it lives here and not on the context.
    std::uint64_t entry = 0;
    while (!(entry = static_cast<std::uint64_t>(call(kAcquire, pool)))) {
        g_spins.fetch_add(1, std::memory_order_relaxed);
        wr64(ctx + 0x120, 0);  // as the game leaves it while it waits
        sched_yield();         // what the game's j_scePthreadYield comes to
    }
    wr64(ctx + 0x120, entry);
    g_borrows.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t obj = rd64(entry + 8);
    const std::int64_t kind = call(kKindOf, obj);
    if (call(kCanUse, target, 3, kind) != 0) {
        // The game leaves here with the entry still taken. Give it back.
        g_error_released.fetch_add(1, std::memory_order_relaxed);
        give_back(pool, ctx, entry);
        return 1;
    }
    // Sixteen bytes, not eight: the game's own slot for this is at rbp-0x40
    // with its stack canary just above at rbp-0x30, and the callee fills the
    // whole of it. An eight-byte local here smashed our stack, which the
    // canary caught on the way out.
    std::uint64_t out[2] = {0, 0};
    if (call(kBegin, obj, obj + 0xb730, target, 0, 3, reinterpret_cast<std::int64_t>(&out[0])) != 0) {
        g_error_released.fetch_add(1, std::memory_order_relaxed);
        give_back(pool, ctx, entry);
        return 1;
    }
    // BBHOST_POOL_WATCH=1 (reconnaissance): the region begin mapped
    // is where this frame's dynamic vertices go; a page watch on it, armed
    // once in the world, names the code that writes them (the watch report
    // at exit).
    // BBHOST_POOL_WATCH=1 watches the region; =0x<address> watches that page
    // instead (the memcpy's source the first watch reported, for example).
    static const std::uint64_t watch = [] {
        const char* e = std::getenv("BBHOST_POOL_WATCH");
        return e && e[0] ? (e[0] == '1' && !e[1] ? 1ull : std::strtoull(e, nullptr, 0)) : 0ull;
    }();
    if (watch && out[0]) {
        static bool armed = false;
        if (!armed && hle_video_flip_count() >= 3000) {
            armed = true;
            const std::uint64_t target = watch == 1 ? out[0] : watch;
            // Direct memory the game writes through its GPU alias, when it has one.
            const std::uint64_t alias = std::getenv("BBHOST_POOL_WATCH_CPU") ? 0 : hle_kernel_gpu_alias(target);
            const std::uint64_t at = alias ? alias : target;
            host_log("pool: watching the borrowed region at 0x%llx (first page%s)", static_cast<ull>(at), alias ? ", the GPU alias" : "");
            hle_watch_arm(at, at + 0x1000);
        }
    }
    call(kHandOver, rd64(ctx + 0x118), static_cast<std::int64_t>(out[0]));
    // The end of the pass, on the object this call took - the game reads it
    // back out of the context here, which is the read that goes wrong.
    const std::uint64_t obj_now = rd64(entry + 8);
    call(kEnd, obj_now, obj_now + 0xb730, rd64(rd64(ctx + 0x118) + 0x40), 0);
    give_back(pool, ctx, entry);
    return 0;
}

}  // namespace

void frame_pool_install(ElfImage* image) {
    g_installed = false;
    if (!image) return;
    if (const char* e = std::getenv("BBHOST_POOL_FIX"); e && e[0] == '0') {
        host_log("pool: the frame loop's borrow is the game's own (BBHOST_POOL_FIX=0)");
        return;
    }
    g_slide = image->mem.slide;
    const std::uint64_t at = g_slide + (kBorrow - kPreferredGuestSlide);
    if (at < g_slide || at + 16 > g_slide + image->mem.size) return;
    auto* p = static_cast<std::uint8_t*>(guest_ptr(image->mem, at));
    if (!p || std::memcmp(p, kPrologue, sizeof(kPrologue)) != 0) {
        host_log("pool: the frame loop's borrow at 0x%llx does not start with the expected prologue; left alone",
                 static_cast<ull>(kBorrow));
        return;
    }
    if (!guest_protect_rwx(&image->mem, at & ~0xfffull, 0x1000)) {
        host_log("pool: cannot unprotect the frame loop's borrow");
        return;
    }
    const std::uint64_t host = reinterpret_cast<std::uint64_t>(thunk_wrap(reinterpret_cast<void*>(&frame_pool_borrow)));
    p[0] = 0x48;  // mov rax, host
    p[1] = 0xb8;
    std::memcpy(p + 2, &host, 8);
    p[10] = 0xff;  // jmp rax
    p[11] = 0xe0;
    guest_protect_rx(&image->mem, at & ~0xfffull, 0x1000);
    g_installed = true;
    host_log("pool: the frame loop's borrow at 0x%llx is ours (the entry stays on the stack)", static_cast<ull>(kBorrow));
}

void frame_pool_report() {
    if (!g_installed) return;
    host_log("pool: %llu borrows, %llu with the context's copy overwritten under them, %llu entries the game's error paths would "
             "have dropped, %llu waits for a free one",
             static_cast<ull>(g_borrows.load()), static_cast<ull>(g_clobbered.load()),
             static_cast<ull>(g_error_released.load()), static_cast<ull>(g_spins.load()));
}
