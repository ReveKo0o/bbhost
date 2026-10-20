// The crash report on Windows: what main.cpp's SIGSEGV handler prints on
// Linux (the faulting pc as a Binary Ninja address when it is guest code, the
// registers, the stack words, the guest's frame-pointer chain, host addresses
// as module+offset for addr2line, the recent command-processor writes), from
// a vectored exception handler that runs last, after the write watch's and
// win_watch's. The unhandled-exception filter behind it ends the process with
// exit code 139 instead of leaving it to the system's crash dialog (or, under
// wine, an auto-attached winedbg that hangs an unattended run).
//
// Host addresses print as "module+offset (link 0x...)": the link address is
// the module's preferred image base plus the offset, which for bbhost.exe
// (linked at 0x600000000 with dynamic relocation off) is the runtime address,
// and what `addr2line -e build-win/bbhost.exe` and llvm-symbolizer take.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
// Installs the handler and the filter. Once, from main, before guest code.
void win_crash_install();
// The eboot's image [lo, hi), for the guest-pc conversion. Before it is
// known every address is a host one.
void win_crash_note_image(std::uint64_t lo, std::uint64_t hi);
// Formats a host address as module+offset (see above) into out; false when it
// lies in no loaded module.
bool win_crash_where(std::uint64_t a, char* out, std::size_t cap);
#endif
