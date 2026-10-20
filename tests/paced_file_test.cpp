// HostPacedFile (core/paced_file.h): what is written in pieces of every size,
// across the chunk boundaries, reads back whole after the commit and replaces
// the file that was there; one never committed, or that cannot be opened,
// leaves the old file and no temporary behind.
#include "core/paced_file.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static std::vector<char> read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int main() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("bbhost-paced-file-test-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path path = dir / "cache.bin", tmp = dir / "cache.bin.tmp";
    std::ofstream(path, std::ios::binary) << "the old file";
    const std::vector<char> old = read_all(path);

    std::vector<char> data(300000 + 1234);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 2654435761u) >> 13);
    {
        HostPacedFile f(path.string(), tmp.string(), 64 << 10);  // a chunk of 64 KiB: synced several times
        std::size_t at = 0;
        for (std::size_t n : {std::size_t{1}, std::size_t{7}, std::size_t{65536}, std::size_t{100000}, std::size_t{65529}}) {
            CHECK(f.write(data.data() + at, n));
            at += n;
            CHECK(fs::exists(tmp) && read_all(path) == old);  // the old file meanwhile
        }
        CHECK(f.write(data.data() + at, data.size() - at));
        CHECK(f.commit());
    }
    CHECK(read_all(path) == data);
    CHECK(!fs::exists(tmp));

    {  // never committed: the file stays as it was, the temporary goes
        HostPacedFile f(path.string(), tmp.string(), 4096);
        CHECK(f.write("not this", 8));
        CHECK(fs::exists(tmp));
    }
    CHECK(read_all(path) == data);
    CHECK(!fs::exists(tmp));

    {  // a temporary that cannot be made
        const fs::path nowhere = dir / "missing" / "cache.bin";
        HostPacedFile f(nowhere.string(), (dir / "missing" / "cache.bin.tmp").string());
        CHECK(!f.write("x", 1));
        CHECK(!f.commit());
        CHECK(!fs::exists(nowhere));
    }

    fs::remove_all(dir, ec);
    if (g_failures) {
        std::fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    std::printf("paced file: ok\n");
    return 0;
}
