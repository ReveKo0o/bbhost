// sceFios2: file/dir handles and the few async ops the game uses, over the
// sandboxed host path mapper. Ops complete synchronously.
#include "core/write_watch.h"
#include "host/frame_stats.h"
#include "hle/common.h"
#include "hle/fs.h"
#include "hle/hle.h"
#include "hle/modules.h"
#include "hle/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

// SCE_FIOS_ERROR_*
constexpr int kFiosOk = 0;
constexpr int kFiosErrBadPath = static_cast<int>(0x80C90007u);
constexpr int kFiosErrBadFh = static_cast<int>(0x80C90011u);
constexpr int kFiosErrBadDh = static_cast<int>(0x80C90012u);
constexpr int kFiosErrBadOp = static_cast<int>(0x80C90013u);
constexpr int kFiosErrAccess = static_cast<int>(0x80C90005u);
constexpr int kFiosErrExists = static_cast<int>(0x80C9000Au);
constexpr int kFiosErrBadArg = static_cast<int>(0x80C90003u);

// SceFiosOpenParams.openFlags
constexpr std::uint32_t kOpenRead = 1;
constexpr std::uint32_t kOpenWrite = 2;
constexpr std::uint32_t kOpenCreate = 0x10;
constexpr std::uint32_t kOpenTruncate = 0x20;
constexpr std::uint32_t kOpenAppend = 0x40;

struct OpenParams {
    std::uint32_t open_flags;
    std::uint32_t op_flags;
    std::uint32_t reserved[2];
    void* buf_ptr;
    std::size_t buf_len;
};

// SceFiosDate is a 64-bit count of microseconds; SceFiosStat is 0x50 bytes.
struct FiosStat {
    std::int64_t file_size;
    std::uint64_t access_date;
    std::uint64_t modification_date;
    std::uint64_t creation_date;
    std::uint32_t stat_flags;  // 1 = directory
    std::uint32_t reserved;
    std::int64_t uid;
    std::int64_t gid;
    std::int64_t dev;
    std::int64_t ino;
    std::int64_t mode;
};
static_assert(sizeof(FiosStat) == 0x50, "SceFiosStat");

struct FiosDirEntry {
    FiosStat stat;
    std::uint16_t offset_to_name;
    std::uint16_t name_length;
    std::uint16_t full_path_length;
    std::uint16_t reserved;
    char full_path[1024];
};

struct File {
    std::FILE* f = nullptr;
    std::string path;
};

struct Dir {
    std::string guest_path;
    std::vector<std::string> names;
    std::size_t pos = 0;
};

struct Op {
    int result = 0;
    std::int64_t actual = 0;
    bool done = true;
};

std::mutex g_mu;
int g_next_fh = 1;
int g_next_dh = 1;
int g_next_op = 1;
std::unordered_map<int, File> g_files;
std::unordered_map<int, Dir> g_dirs;
std::unordered_map<int, Op> g_ops;

std::uint64_t to_fios_date(std::int64_t unix_sec) {
    return unix_sec > 0 ? static_cast<std::uint64_t>(unix_sec) * 1000000ull : 0;
}

bool host_stat(const std::string& host, FiosStat* out) {
    struct stat st{};
    if (::stat(host.c_str(), &st) != 0) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->file_size = S_ISDIR(st.st_mode) ? 0 : static_cast<std::int64_t>(st.st_size);
    out->access_date = to_fios_date(st.st_atime);
    out->modification_date = to_fios_date(st.st_mtime);
    out->creation_date = to_fios_date(st.st_ctime);
    out->stat_flags = S_ISDIR(st.st_mode) ? 1u : 0u;
    out->dev = static_cast<std::int64_t>(st.st_dev);
    out->ino = static_cast<std::int64_t>(st.st_ino);
    out->mode = static_cast<std::int64_t>(st.st_mode);
    return true;
}

int make_op(int result, std::int64_t actual) {
    std::lock_guard<std::mutex> lock(g_mu);
    int id = g_next_op++;
    g_ops[id] = Op{result, actual, true};
    return id;
}

std::string mode_for(std::uint32_t flags) {
    const bool w = flags & kOpenWrite;
    const bool r = flags & kOpenRead;
    if (flags & kOpenAppend) {
        return r ? "a+b" : "ab";
    }
    if (w && ((flags & kOpenTruncate) || !r)) {
        return r ? "w+b" : "wb";
    }
    if (w) {
        return "r+b";
    }
    return "rb";
}

// int sceFiosFHOpenSync(const SceFiosOpAttr*, SceFiosFH* out, const char* path, const SceFiosOpenParams*)
GUEST_ABI int hle_fios_fh_open_sync(const void*, int* out, const char* path, const OpenParams* params) {
    if (!out || !path) {
        return kFiosErrBadArg;
    }
    *out = 0;
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return kFiosErrBadPath;
    }
    const std::uint32_t flags = params ? params->open_flags : kOpenRead;
    if ((flags & kOpenWrite) && (flags & kOpenCreate) == 0) {
        std::FILE* probe = std::fopen(host.c_str(), "rb");
        if (!probe) {
            return kFiosErrBadPath;
        }
        std::fclose(probe);
    }
    std::FILE* f = std::fopen(host.c_str(), mode_for(flags).c_str());
    if (!f) {
        static int logs = 0;
        if (logs < 16) {
            ++logs;
            host_log("sceFiosFHOpenSync %s (flags 0x%x) failed", path, flags);
        }
        return kFiosErrBadPath;
    }
    int id;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        id = g_next_fh++;
        g_files[id] = File{f, host};
    }
    *out = id;
    static int logs = 0;
    if (logs < 16) {
        ++logs;
        host_log("sceFiosFHOpenSync %s -> fh %d", path, id);
    }
    return kFiosOk;
}

GUEST_ABI int hle_fios_fh_close_sync(const void*, int fh) {
    std::FILE* f = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_files.find(fh);
        if (it == g_files.end()) {
            return kFiosErrBadFh;
        }
        f = it->second.f;
        g_files.erase(it);
    }
    std::fclose(f);
    return kFiosOk;
}

std::FILE* file_of(int fh) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_files.find(fh);
    return it == g_files.end() ? nullptr : it->second.f;
}

// SceFiosSize sceFiosFHReadSync(attr, fh, void* buf, SceFiosSize len)
GUEST_ABI std::int64_t hle_fios_fh_read_sync(const void*, int fh, void* buf, std::int64_t len) {
    std::FILE* f = file_of(fh);
    if (!f) {
        return kFiosErrBadFh;
    }
    if (!buf || len < 0) {
        return kFiosErrBadArg;
    }
    std::size_t n = write_watch_fread(buf, 1, static_cast<std::size_t>(len), f);
    return static_cast<std::int64_t>(n);
}

GUEST_ABI std::int64_t hle_fios_fh_write_sync(const void*, int fh, const void* buf, std::int64_t len) {
    std::FILE* f = file_of(fh);
    if (!f) {
        return kFiosErrBadFh;
    }
    if (!buf || len < 0) {
        return kFiosErrBadArg;
    }
    std::size_t n = std::fwrite(buf, 1, static_cast<std::size_t>(len), f);
    return static_cast<std::int64_t>(n);
}

// SceFiosOffset sceFiosFHSeek(fh, offset, whence)  whence: 0 set, 1 cur, 2 end
GUEST_ABI std::int64_t hle_fios_fh_seek(int fh, std::int64_t off, int whence) {
    std::FILE* f = file_of(fh);
    if (!f) {
        return kFiosErrBadFh;
    }
    int w = whence == 0 ? SEEK_SET : (whence == 1 ? SEEK_CUR : SEEK_END);
#if defined(_WIN32)
    if (_fseeki64(f, off, w) != 0) {
        return kFiosErrBadArg;
    }
    return _ftelli64(f);
#else
    if (fseeko(f, static_cast<off_t>(off), w) != 0) {
        return kFiosErrBadArg;
    }
    return static_cast<std::int64_t>(ftello(f));
#endif
}

GUEST_ABI std::int64_t hle_fios_fh_tell(int fh) {
    std::FILE* f = file_of(fh);
    if (!f) {
        return kFiosErrBadFh;
    }
#if defined(_WIN32)
    return _ftelli64(f);
#else
    return static_cast<std::int64_t>(ftello(f));
#endif
}

GUEST_ABI int hle_fios_stat_sync(const void*, const char* path, FiosStat* out) {
    if (!path || !out) {
        return kFiosErrBadArg;
    }
    std::string host = hle_fs_map_path(path);
    if (host.empty() || !host_stat(host, out)) {
        return kFiosErrBadPath;
    }
    return kFiosOk;
}

GUEST_ABI int hle_fios_file_delete_sync(const void*, const char* path) {
    if (!path) {
        return kFiosErrBadArg;
    }
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return kFiosErrBadPath;
    }
    return std::remove(host.c_str()) == 0 ? kFiosOk : kFiosErrBadPath;
}

// SceFiosOp sceFiosDirectoryCreate(attr, path): async; completes immediately.
GUEST_ABI int hle_fios_directory_create(const void*, const char* path) {
    if (!path) {
        return make_op(kFiosErrBadArg, 0);
    }
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return make_op(kFiosErrBadPath, 0);
    }
#if defined(_WIN32)
    int r = _mkdir(host.c_str());
#else
    int r = ::mkdir(host.c_str(), 0777);
#endif
    if (r != 0 && errno == EEXIST) {
        return make_op(kFiosErrExists, 0);
    }
    return make_op(r == 0 ? kFiosOk : kFiosErrAccess, 0);
}

int dh_open_impl(int* out, const char* path) {
    if (!out || !path) {
        return kFiosErrBadArg;
    }
    *out = 0;
    std::string host = hle_fs_map_path(path);
    if (host.empty()) {
        return kFiosErrBadPath;
    }
    Dir d;
    d.guest_path = path;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((host + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return kFiosErrBadPath;
    }
    do {
        if (std::strcmp(fd.cFileName, ".") && std::strcmp(fd.cFileName, "..")) {
            d.names.push_back(fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* dir = ::opendir(host.c_str());
    if (!dir) {
        return kFiosErrBadPath;
    }
    while (dirent* e = ::readdir(dir)) {
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
            d.names.push_back(e->d_name);
        }
    }
    ::closedir(dir);
#endif
    std::lock_guard<std::mutex> lock(g_mu);
    int id = g_next_dh++;
    g_dirs[id] = std::move(d);
    *out = id;
    return kFiosOk;
}

// int sceFiosDHOpenSync(attr, SceFiosDH* out, const char* path, SceFiosBuffer buf{ptr,len})
GUEST_ABI int hle_fios_dh_open_sync(const void*, int* out, const char* path, void*, std::size_t) {
    return dh_open_impl(out, path);
}

// SceFiosOp sceFiosDHOpen(attr, out, path, buf): async form.
GUEST_ABI int hle_fios_dh_open(const void*, int* out, const char* path, void*, std::size_t) {
    return make_op(dh_open_impl(out, path), 0);
}

GUEST_ABI int hle_fios_dh_read_sync(const void*, int dh, FiosDirEntry* out) {
    if (!out) {
        return kFiosErrBadArg;
    }
    std::string guest_dir;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_dirs.find(dh);
        if (it == g_dirs.end()) {
            return kFiosErrBadDh;
        }
        Dir& d = it->second;
        if (d.pos >= d.names.size()) {
            return static_cast<int>(0x80C9001Fu);  // SCE_FIOS_ERROR_EOF
        }
        guest_dir = d.guest_path;
        name = d.names[d.pos++];
    }
    std::string full = guest_dir;
    if (full.empty() || full.back() != '/') {
        full += '/';
    }
    full += name;
    std::memset(out, 0, sizeof(*out));
    std::string host = hle_fs_map_path(full.c_str());
    if (!host.empty()) {
        host_stat(host, &out->stat);
    }
    std::strncpy(out->full_path, full.c_str(), sizeof(out->full_path) - 1);
    out->full_path_length = static_cast<std::uint16_t>(full.size());
    out->offset_to_name = static_cast<std::uint16_t>(full.size() - name.size());
    out->name_length = static_cast<std::uint16_t>(name.size());
    return kFiosOk;
}

GUEST_ABI int hle_fios_dh_close_sync(const void*, int dh) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_dirs.erase(dh) ? kFiosOk : kFiosErrBadDh;
}

GUEST_ABI int hle_fios_op_wait(int op) {
    MainThreadWait timed(2);
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_ops.find(op);
    return it == g_ops.end() ? kFiosErrBadOp : it->second.result;
}

GUEST_ABI std::int64_t hle_fios_op_get_actual_count(int op) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_ops.find(op);
    return it == g_ops.end() ? 0 : it->second.actual;
}

GUEST_ABI void hle_fios_op_delete(int op) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_ops.erase(op);
}

}  // namespace

void hle_register_fios() {
#define REG(name, fn) register_hle_fn(name, reinterpret_cast<void*>(fn))
    REG("sceFiosFHOpenSync", hle_fios_fh_open_sync);
    REG("sceFiosFHCloseSync", hle_fios_fh_close_sync);
    REG("sceFiosFHReadSync", hle_fios_fh_read_sync);
    REG("sceFiosFHWriteSync", hle_fios_fh_write_sync);
    REG("sceFiosFHSeek", hle_fios_fh_seek);
    REG("sceFiosFHTell", hle_fios_fh_tell);
    REG("sceFiosStatSync", hle_fios_stat_sync);
    REG("sceFiosFileDeleteSync", hle_fios_file_delete_sync);
    REG("sceFiosDirectoryCreate", hle_fios_directory_create);
    REG("sceFiosDHOpen", hle_fios_dh_open);
    REG("sceFiosDHOpenSync", hle_fios_dh_open_sync);
    REG("sceFiosDHReadSync", hle_fios_dh_read_sync);
    REG("sceFiosDHCloseSync", hle_fios_dh_close_sync);
    REG("sceFiosOpWait", hle_fios_op_wait);
    REG("sceFiosOpGetActualCount", hle_fios_op_get_actual_count);
    REG("sceFiosOpDelete", hle_fios_op_delete);
#undef REG
}
