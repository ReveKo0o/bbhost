#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>
#include <unordered_map>

#include "hle/common.h"
#include "hle/libc_errno.h"
#include "hle/sysv_va.h"

namespace bb::libc {

inline constexpr unsigned read_access = 1, write_access = 2;
struct FileAccesses {
    std::mutex mutex;
    std::unordered_map<FILE*, unsigned> entries;
};
inline FileAccesses& file_accesses() {
    static FileAccesses accesses;
    return accesses;
}
inline bool remember_file(FILE* file, const char* mode) {
    unsigned access = *mode == 'r' ? read_access : write_access;
    bool binary = false;
    for (const char* p = mode + 1; *p == '+' || *p == 'b'; ++p) {
        if (*p == '+') {
            if (access == (read_access | write_access)) break;
            access = read_access | write_access;
        } else {
            if (binary) break;
            binary = true;
        }
    }
    auto& files = file_accesses();
    std::lock_guard lock(files.mutex);
    try { files.entries[file] = access; }
    catch (const std::bad_alloc&) { return false; }
    return true;
}
inline void forget_file(FILE* file) {
    auto& files = file_accesses();
    std::lock_guard lock(files.mutex);
    files.entries.erase(file);
}
inline unsigned file_access(FILE* file) {
    auto& files = file_accesses();
    std::lock_guard lock(files.mutex);
    const auto at = files.entries.find(file);
    if (at != files.entries.end()) return at->second;
    if (file == stdin) return read_access;
    if (file == stdout || file == stderr) return write_access;
    return read_access | write_access;
}
inline FILE* opened_file(FILE* file, const char* mode) {
    if (!file) set_guest_errno_host(errno);
    else if (!remember_file(file, mode)) {
        std::fclose(file);
        set_guest_errno_host(ENOMEM);
        return nullptr;
    }
    return file;
}
inline FILE* open_file(const char* path, const char* mode) {
    return opened_file(std::fopen(path, mode), mode);
}
inline FILE* reopen_file(const char* path, const char* mode, FILE* file) {
    forget_file(file);
    return opened_file(std::freopen(path, mode, file), mode);
}
inline int close_file(FILE* file) {
    forget_file(file);
    return libc_result(std::fclose(file));
}
template <typename Fn> inline auto file_result(FILE* file, Fn&& call, unsigned access) {
    errno = 0;
    auto result = call();
    const int error = errno;
    // EOF/short transfers are not necessarily errors; don't leak old host
    // errno or erase the guest's previous errno on successful operations.
    // Native direction checks set ferror but leave errno alone. Host CRTs
    // commonly set EBADF here; propagate only for an allowed direction.
    if (error && std::ferror(file) && (file_access(file) & access)) set_guest_errno_host(error);
    return result;
}

inline int print_file(FILE* file, const char* format, SysvVaList* ap) {
    return file_result(file, [&] { return sysv_vfprintf(file, format, ap); }, write_access);
}
inline int scan_file(FILE* file, const char* format, SysvVaList* ap) {
    return file_result(file, [&] {
        const int result = sysv_vfscanf(file, format, ap);
        // glibc's scanf can reject a write-only stream before its getc
        // path, leaving ferror clear. Native fscanf reads through fgetc,
        // whose direction rejection sets ferror without changing errno.
        if (result == EOF && errno == EBADF && !std::ferror(file)) {
            const int error = errno;
            (void)std::fgetc(file);
            errno = error;
        }
        return result;
    }, read_access);
}

inline void print_error(FILE* file, const char* prefix) {
    if (prefix && *prefix) {
        file_result(file, [&] { return std::fputs(prefix, file); }, write_access);
        file_result(file, [&] { return std::fputs(": ", file); }, write_access);
    }
    // Native perror reads guest errno AFTER writing its optional prefix.
    const char* message = error_string(t_guest_errno);
    file_result(file, [&] { return std::fputs(message, file); }, write_access);
    file_result(file, [&] { return std::fputc('\n', file); }, write_access);
}

struct Fpos {
    std::int64_t offset;
    std::uint64_t state[2];
};
static_assert(sizeof(Fpos) == 24);

inline int seek(FILE* file, std::int64_t offset, int whence) {
#if defined(_WIN32)
    return _fseeki64(file, offset, whence);
#else
    static_assert(sizeof(off_t) == 8);
    return ::fseeko(file, static_cast<off_t>(offset), whence);
#endif
}
inline std::int64_t tell(FILE* file) {
#if defined(_WIN32)
    return _ftelli64(file);
#else
    return static_cast<std::int64_t>(::ftello(file));
#endif
}
inline int get_position(FILE* file, Fpos* position) {
    const auto offset = tell(file);
    if (offset == -1) return -1;
    // Guest file I/O currently implements narrow streams only; no pending
    // multibyte conversion state. Never expose the host's opaque fpos_t.
    *position = {offset, {0, 0}};
    return 0;
}

}  // namespace bb::libc
