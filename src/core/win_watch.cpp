#include "core/win_watch.h"

#if defined(_WIN32)

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace {

std::atomic<std::uintptr_t> g_lo{0}, g_hi{0}, g_page_lo{0}, g_page_hi{0};
std::atomic<int> g_hits{0}, g_max{0};
std::atomic<bool> g_armed{false};
std::uint64_t (*g_slide)() = nullptr;
thread_local bool t_stepping = false;
thread_local std::uintptr_t t_fault_addr = 0;
PVOID g_handler = nullptr;

void protect_pages(DWORD prot) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(g_page_lo.load()), g_page_hi.load() - g_page_lo.load(), prot, &old);
}

LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* ctx = ep->ContextRecord;
    if (code == EXCEPTION_ACCESS_VIOLATION && g_armed.load()) {
        const std::uintptr_t addr = static_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        if (addr >= g_page_lo.load() && addr < g_page_hi.load()) {
            if (write && addr >= g_lo.load() && addr < g_hi.load() && g_hits.fetch_add(1) < g_max.load()) {
                const std::uint64_t slide = g_slide ? g_slide() : 0;
                const std::uint64_t rip = ctx->Rip;
                host_log("win-watch: write to 0x%llx from %s0x%llx (rax=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx)",
                         static_cast<unsigned long long>(addr), rip >= slide && slide ? "guest " : "host ",
                         static_cast<unsigned long long>(rip >= slide && slide ? rip - slide + 0x400000 : rip),
                         static_cast<unsigned long long>(ctx->Rax), static_cast<unsigned long long>(ctx->Rcx),
                         static_cast<unsigned long long>(ctx->Rdx), static_cast<unsigned long long>(ctx->Rsi),
                         static_cast<unsigned long long>(ctx->Rdi));
            }
            // Let the instruction through: pages writable, single-step, re-protect.
            protect_pages(PAGE_READWRITE);
            t_stepping = true;
            t_fault_addr = addr;
            ctx->EFlags |= 0x100;  // TF
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && t_stepping) {
        t_stepping = false;
        ctx->EFlags &= ~static_cast<DWORD>(0x100);
        if (g_armed.load()) protect_pages(PAGE_READONLY);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool win_watch_arm(std::uintptr_t lo, std::uintptr_t hi, int max_hits) {
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, handler);
    if (!g_handler) return false;
    g_lo = lo;
    g_hi = hi;
    g_page_lo = lo & ~static_cast<std::uintptr_t>(0xfff);
    g_page_hi = (hi + 0xfff) & ~static_cast<std::uintptr_t>(0xfff);
    g_hits = 0;
    g_max = max_hits;
    g_armed = true;
    protect_pages(PAGE_READONLY);
    host_log("win-watch: armed on [0x%llx, 0x%llx) (pages [0x%llx, 0x%llx))", static_cast<unsigned long long>(lo),
             static_cast<unsigned long long>(hi), static_cast<unsigned long long>(g_page_lo.load()),
             static_cast<unsigned long long>(g_page_hi.load()));
    return true;
}

void win_watch_disarm() {
    if (!g_armed.exchange(false)) return;
    protect_pages(PAGE_READWRITE);
}

#endif
