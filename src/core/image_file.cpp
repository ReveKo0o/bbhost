#include "core/image_file.h"

#include <zlib.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// zlib's fastest level: an F12 capture of a world frame (69 images, 104
// million pixels) in 4.4 s and 65 MB, where level 6 took 13.6 s for 59 MB.
constexpr int kLevel = 1;

void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

bool is_png(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".png";
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// The filtered bytes' sum as signed values: the row whose sum is smallest
// usually deflates smallest (the PNG specification's own heuristic).
std::uint64_t cost(const std::uint8_t* v, std::size_t n) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += v[i] < 128 ? v[i] : 256 - v[i];
    return sum;
}

}  // namespace

struct ImageFile::State {
    std::FILE* f = nullptr;
    bool png = false, ok = true;
    std::uint32_t width = 0, height = 0, rows = 0;
    // PNG: the row above (unfiltered), a row under each of the five filters
    // (the filter's byte first), and zlib's stream with its output buffer.
    std::vector<std::uint8_t> above, filtered[5], out;
    z_stream z{};
    bool z_open = false;

    void chunk(const char* type, const std::uint8_t* data, std::size_t len) {
        std::uint8_t head[8], crc_bytes[4];
        put32(head, static_cast<std::uint32_t>(len));
        std::memcpy(head + 4, type, 4);
        uLong crc = crc32(0, head + 4, 4);
        if (len) crc = crc32(crc, data, static_cast<uInt>(len));
        put32(crc_bytes, static_cast<std::uint32_t>(crc));
        ok = ok && std::fwrite(head, 1, 8, f) == 8 && (!len || std::fwrite(data, 1, len, f) == len) &&
             std::fwrite(crc_bytes, 1, 4, f) == 4;
    }
    // What zlib has produced, as one IDAT chunk.
    void emit() {
        const std::size_t have = out.size() - z.avail_out;
        if (have) chunk("IDAT", out.data(), have);
        z.next_out = out.data();
        z.avail_out = static_cast<uInt>(out.size());
    }
    // Deflates n bytes; Z_FINISH ends the stream and writes its last chunk.
    void feed(const std::uint8_t* in, std::size_t n, int flush) {
        z.next_in = const_cast<Bytef*>(in);
        z.avail_in = static_cast<uInt>(n);
        int r;
        do {
            r = deflate(&z, flush);
            if (r == Z_STREAM_ERROR) {
                ok = false;
                return;
            }
            if (z.avail_out == 0) emit();
        } while (flush == Z_FINISH ? r != Z_STREAM_END : z.avail_in != 0);
        if (flush == Z_FINISH) emit();
    }
};

ImageFile::ImageFile() : s_(std::make_unique<State>()) {}

ImageFile::~ImageFile() {
    if (s_->f) close();
}

bool ImageFile::open(const std::string& path, std::uint32_t width, std::uint32_t height) {
    if (s_->f) close();
    s_ = std::make_unique<State>();
    State& s = *s_;
    if (!width || !height) return false;
    s.f = std::fopen(path.c_str(), "wb");
    if (!s.f) return false;
    s.width = width;
    s.height = height;
    s.png = is_png(path);
    if (!s.png) {
        s.ok = std::fprintf(s.f, "P6\n%u %u\n255\n", width, height) > 0;
        return s.ok;
    }
    static const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    s.ok = std::fwrite(signature, 1, 8, s.f) == 8;
    std::uint8_t ihdr[13];
    put32(ihdr, width);
    put32(ihdr + 4, height);
    ihdr[8] = 8;                          // bits per channel
    ihdr[9] = 2;                          // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;   // deflate, the five filters, no interlace
    s.chunk("IHDR", ihdr, sizeof(ihdr));
    if (deflateInit(&s.z, kLevel) != Z_OK) {
        std::fclose(s.f);
        s.f = nullptr;
        return false;
    }
    s.z_open = true;
    s.out.resize(256 * 1024);
    s.z.next_out = s.out.data();
    s.z.avail_out = static_cast<uInt>(s.out.size());
    const std::size_t n = static_cast<std::size_t>(width) * 3;
    s.above.assign(n, 0);
    for (std::uint8_t k = 0; k < 5; ++k) {
        s.filtered[k].resize(n + 1);
        s.filtered[k][0] = k;
    }
    return s.ok;
}

bool ImageFile::row(const std::uint8_t* rgb) {
    State& s = *s_;
    if (!s.f || s.rows >= s.height) return false;
    ++s.rows;
    const std::size_t n = static_cast<std::size_t>(s.width) * 3;
    if (!s.png) {
        s.ok = s.ok && std::fwrite(rgb, 1, n, s.f) == n;
        return s.ok;
    }
    // Each filter predicts a byte from the one a pixel to the left (a), the
    // one above (b) and the one above that left one (c); the row keeps the
    // differences.
    const std::uint8_t* up = s.above.data();
    std::uint8_t* none = s.filtered[0].data() + 1;
    std::uint8_t* sub = s.filtered[1].data() + 1;
    std::uint8_t* upf = s.filtered[2].data() + 1;
    std::uint8_t* avg = s.filtered[3].data() + 1;
    std::uint8_t* pth = s.filtered[4].data() + 1;
    std::memcpy(none, rgb, n);
    for (std::size_t i = 0; i < n; ++i) {
        const int a = i >= 3 ? rgb[i - 3] : 0, b = up[i], c = i >= 3 ? up[i - 3] : 0;
        sub[i] = static_cast<std::uint8_t>(rgb[i] - a);
        upf[i] = static_cast<std::uint8_t>(rgb[i] - b);
        avg[i] = static_cast<std::uint8_t>(rgb[i] - ((a + b) >> 1));
        pth[i] = static_cast<std::uint8_t>(rgb[i] - paeth(a, b, c));
    }
    int best = 0;
    std::uint64_t best_cost = cost(none, n);
    for (int k = 1; k < 5; ++k) {
        const std::uint64_t c = cost(s.filtered[k].data() + 1, n);
        if (c < best_cost) {
            best_cost = c;
            best = k;
        }
    }
    s.feed(s.filtered[best].data(), n + 1, Z_NO_FLUSH);
    std::memcpy(s.above.data(), rgb, n);
    return s.ok;
}

bool ImageFile::close() {
    State& s = *s_;
    if (!s.f) return false;
    if (s.png) {
        if (s.z_open) {
            s.feed(nullptr, 0, Z_FINISH);
            deflateEnd(&s.z);
            s.z_open = false;
        }
        s.chunk("IEND", nullptr, 0);
    }
    s.ok = std::fclose(s.f) == 0 && s.ok;
    s.f = nullptr;
    return s.ok && s.rows == s.height;
}
