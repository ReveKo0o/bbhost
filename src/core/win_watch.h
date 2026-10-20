// A page write watch on Windows (a vectored exception handler over
// VirtualProtect): the foundation the Linux write-watch (core/write_watch.h)
// has in SIGSEGV + mprotect. Today a diagnostic: BBHOST_SAVE_DUMP's search for
// what clears the save directory prefix. Nothing on Linux.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
// Logs every write that lands in [lo, hi) with the writing instruction's
// address (an ELF VA when it is guest code), by protecting the page(s) and
// single-stepping the faulting instruction. Up to `max_hits` lines.
bool win_watch_arm(std::uintptr_t lo, std::uintptr_t hi, int max_hits);
void win_watch_disarm();
#endif
