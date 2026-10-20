// Drives the sceAjm HLE the way FMOD's AT9 codec in the eboot does: init,
// control 0x6001 (initialize+reset with the 4-byte config), run 0x801 (codec
// info), then run 0x800000001001 (stream sideband + multiple frames) over a
// stream pulled from an FSB5 sound bank in the game dump. Skips when the dump
// is not present.
#include "test_app0.h"
#include "guest_abi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

void hle_register_ajm();

namespace {
std::map<std::string, void*> g_fns;
}
void register_hle_fn(const char* name, void* fn) { g_fns[name] = fn; }
template <typename F>
F fn(const char* name) {
    auto it = g_fns.find(name);
    if (it == g_fns.end()) {
        std::fprintf(stderr, "missing %s\n", name);
        std::exit(1);
    }
    return reinterpret_cast<F>(it->second);
}

static std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
static std::uint64_t rd64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

int main(int argc, char** argv) {
    const std::string path_s = argc > 1 ? std::string(argv[1]) : test_app0_file("dvdroot_ps4/sound/sprj_c5120.fsb");
    const char* path = path_s.c_str();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("at9_test skipped: %s not readable\n", path);
        return kTestSkip;
    }
    std::vector<std::uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() < 0x40 || std::memcmp(f.data(), "FSB5", 4) != 0 || rd32(f.data() + 0x18) != 0xd) {
        std::fprintf(stderr, "not an FSB5 ATRAC9 bank\n");
        return 1;
    }
    const std::uint32_t version = rd32(f.data() + 4);
    const std::uint32_t nsamples = rd32(f.data() + 8);
    const std::uint32_t shs = rd32(f.data() + 0xc);
    const std::uint32_t nts = rd32(f.data() + 0x10);
    const std::uint32_t ds = rd32(f.data() + 0x14);
    const std::size_t hdr = version == 1 ? 0x3c : 0x40;
    const std::size_t data_start = hdr + shs + nts;
    // First sample: mode word, then extra chunks; type 9 carries the AT9 config.
    std::size_t off = hdr;
    const std::uint64_t mode = rd64(f.data() + off);
    off += 8;
    const int channels = static_cast<int>(((mode >> 5) & 1) + 1);
    const std::uint64_t doff = ((mode >> 7) & 0x7ffffff) << 5;
    std::uint8_t cfg[4] = {0, 0, 0, 0};
    bool have_cfg = false;
    for (bool extra = (mode & 1) != 0; extra;) {
        const std::uint32_t c = rd32(f.data() + off);
        off += 4;
        extra = (c & 1) != 0;
        const std::uint32_t size = (c >> 1) & 0xffffff;
        const std::uint32_t type = (c >> 25) & 0x7f;
        if (type == 9 && size >= 8) {
            std::memcpy(cfg, f.data() + off + 4, 4);
            have_cfg = true;
        }
        off += size;
    }
    std::uint64_t next = ds;
    if (nsamples > 1) {
        next = ((rd64(f.data() + off) >> 7) & 0x7ffffff) << 5;
    }
    if (!have_cfg || next <= doff || data_start + next > f.size()) {
        std::fprintf(stderr, "could not locate the first ATRAC9 stream\n");
        return 1;
    }
    const std::uint8_t* stream = f.data() + data_start + doff;
    const std::size_t stream_len = static_cast<std::size_t>(next - doff);

    hle_register_ajm();
    auto init = fn<int (GUEST_ABI*)(std::int64_t, unsigned*)>("sceAjmInitialize");
    auto modreg = fn<int (GUEST_ABI*)(unsigned, int, std::int64_t)>("sceAjmModuleRegister");
    auto create = fn<int (GUEST_ABI*)(unsigned, int, std::uint64_t, unsigned*)>("sceAjmInstanceCreate");
    auto control = fn<void* (GUEST_ABI*)(void*, unsigned, std::uint64_t, void*, std::uint64_t, void*, std::uint64_t, void*)>(
        "sceAjmBatchJobControlBufferRa");
    auto run = fn<void* (GUEST_ABI*)(void*, unsigned, std::uint64_t, void*, std::uint64_t, void*, std::uint64_t, void*,
                                     std::uint64_t, void*)>("sceAjmBatchJobRunBufferRa");
    auto start = fn<int (GUEST_ABI*)(unsigned, void*, std::uint64_t, int, void*, unsigned*)>("sceAjmBatchStartBuffer");
    auto wait = fn<int (GUEST_ABI*)(unsigned, unsigned, unsigned, void*)>("sceAjmBatchWait");

    unsigned ctx = 0, inst = 0, batch = 0;
    if (init(0, &ctx) != 0 || modreg(ctx, 1, 0) != 0 ||
        create(ctx, 1, 0x409 | (static_cast<std::uint64_t>(channels) << 3), &inst) != 0) {
        std::fprintf(stderr, "setup failed\n");
        return 1;
    }
    alignas(16) std::uint8_t buf[0x120];
    std::uint8_t err[0x40];
    std::uint8_t init_param[8] = {cfg[0], cfg[1], cfg[2], cfg[3], 0, 0, 0, 0};
    std::uint8_t init_side[8];
    std::uint8_t info_side[0x18];
    std::uint8_t* p = buf;
    p = static_cast<std::uint8_t*>(control(p, inst, 0x6001, init_param, 8, init_side, 8, nullptr));
    p = static_cast<std::uint8_t*>(run(p, inst, 0x801, nullptr, 0, nullptr, 0, info_side, 0x18, nullptr));
    if (start(ctx, buf, static_cast<std::uint64_t>(p - buf), 0x5a, err, &batch) != 0 || wait(ctx, batch, ~0u, err) != 0) {
        std::fprintf(stderr, "init batch failed\n");
        return 1;
    }
    if (rd32(init_side) != 0 || rd32(info_side) != 0) {
        std::fprintf(stderr, "init result %u / info result %u\n", rd32(init_side), rd32(info_side));
        return 1;
    }
    const std::uint32_t superframe = rd32(info_side + 8);
    const std::uint32_t frames_per_sf = rd32(info_side + 12);
    const std::uint32_t frame_samples = rd32(info_side + 16);
    std::printf("config %02x%02x%02x%02x: superframe %u bytes, %u frames of %u samples\n", cfg[0], cfg[1], cfg[2],
                cfg[3], superframe, frames_per_sf, frame_samples);
    if (superframe == 0 || frames_per_sf == 0 || frame_samples == 0) {
        return 1;
    }

    // Decode like FMOD: up to 3 superframes in, channels*4096 bytes out.
    const std::size_t in_len = std::min<std::size_t>(stream_len, static_cast<std::size_t>(superframe) * 3);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(channels) * 2048);
    std::uint8_t side[0x20];
    p = buf;
    p = static_cast<std::uint8_t*>(run(p, inst, 0x800000001001ull, const_cast<std::uint8_t*>(stream), in_len, pcm.data(),
                                       pcm.size() * 2, side, 0x20, nullptr));
    if (start(ctx, buf, static_cast<std::uint64_t>(p - buf), 0x5a, err, &batch) != 0 || wait(ctx, batch, ~0u, err) != 0) {
        std::fprintf(stderr, "decode batch failed\n");
        return 1;
    }
    const std::uint32_t result = rd32(side);
    const std::uint32_t consumed = rd32(side + 8);
    const std::uint32_t produced = rd32(side + 12);
    const std::uint64_t total = rd64(side + 16);
    const std::uint32_t frames = rd32(side + 24);
    std::printf("decode: result %u consumed %u produced %u total %llu frames %u\n", result, consumed, produced,
                static_cast<unsigned long long>(total), frames);
    const std::uint32_t expect_frames = static_cast<std::uint32_t>(2048 / frame_samples);
    // PARTIAL_INPUT / NOT_ENOUGH_ROOM say where a run stopped; FMOD takes
    // anything else as a failure.
    if ((result & ~0x18u) != 0 || frames != expect_frames || produced != frames * frame_samples * static_cast<std::uint32_t>(channels) * 2 ||
        consumed == 0 || consumed > in_len) {
        std::fprintf(stderr, "unexpected decode accounting (expected %u frames)\n", expect_frames);
        return 1;
    }
    long energy = 0;
    for (std::size_t i = 0; i < produced / 2; ++i) {
        energy += std::abs(static_cast<int>(pcm[i]));
    }
    if (energy == 0) {
        std::fprintf(stderr, "decoded silence\n");
        return 1;
    }
    // FMOD's seek (0x1119160): reset with a gapless window - 1024 samples to
    // emit after skipping 256 - then decode. Without the instance's
    // gapless-loop flag the run stops at the window; with it (FMOD's flags)
    // the window starts over and decoding goes on.
    unsigned looping = inst;
    if (create(ctx, 1, 0x009 | (static_cast<std::uint64_t>(channels) << 3), &inst) != 0) return 1;
    const auto decode = [&](std::uint64_t control_flags, const void* side_in, std::uint64_t side_in_sz, std::uint32_t& produced_out,
                            std::uint32_t& result_out) {
        std::uint8_t ctl_side[0x10] = {};
        std::uint8_t run_side[0x20] = {};
        std::uint8_t* q = buf;
        q = static_cast<std::uint8_t*>(control(q, inst, control_flags, const_cast<void*>(side_in), side_in_sz, ctl_side, 8, nullptr));
        q = static_cast<std::uint8_t*>(run(q, inst, 0x800000001001ull, const_cast<std::uint8_t*>(stream), in_len, pcm.data(),
                                           pcm.size() * 2, run_side, 0x20, nullptr));
        if (start(ctx, buf, static_cast<std::uint64_t>(q - buf), 0x5a, err, &batch) != 0 || wait(ctx, batch, ~0u, err) != 0) {
            return false;
        }
        result_out = rd32(ctl_side) | rd32(run_side);
        produced_out = rd32(run_side + 12);
        return true;
    };
    const std::uint32_t sample_bytes = static_cast<std::uint32_t>(channels) * 2;
    std::uint8_t gapless[8] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};  // total 1024, skip 256
    std::uint32_t got = 0, res = 0;
    if (!decode(0x6001, init_param, 8, got, res)) return 1;  // initialize the new instance
    if (!decode(0x200000002001ull, gapless, 8, got, res) || (res & ~0x18u) != 0 || got != 1024 * sample_bytes) {
        std::fprintf(stderr, "gapless decode: result %u, produced %u (want %u)\n", res, got, 1024 * sample_bytes);
        return 1;
    }
    // FMOD reuses the instance for the next sound: initialize + reset, no
    // gapless window. It must decode in full, not stop at the old window.
    if (!decode(0x6001, init_param, 8, got, res) || (res & ~0x18u) != 0 || got != 2048 * sample_bytes) {
        std::fprintf(stderr, "reused instance: result %u, produced %u (want %u) - the old gapless window survived the reset\n",
                     res, got, 2048 * sample_bytes);
        return 1;
    }
    // The same window on FMOD's looping instance: past 1024 samples it starts
    // over, so the three superframes fill more than the window.
    std::swap(inst, looping);
    if (!decode(0x200000002001ull, gapless, 8, got, res) || (res & ~0x18u) != 0 || got <= 1024 * sample_bytes) {
        std::fprintf(stderr, "gapless loop: result %u, produced %u (want more than %u)\n", res, got, 1024 * sample_bytes);
        return 1;
    }

    // FMOD's seek in stream mode (0x1119160): three superframes in, its whole
    // 2048-sample ring out, 256 samples skipped - nine frames decoded, the
    // ring filled, one frame of the third superframe consumed.
    {
        std::uint8_t seek_gapless[8] = {0x00, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};  // total 16384, skip 256
        std::uint8_t ctl_side[0x10] = {};
        std::uint8_t run_side[0x20] = {};
        std::uint8_t* q = buf;
        q = static_cast<std::uint8_t*>(control(q, inst, 0x200000002001ull, seek_gapless, 8, ctl_side, 8, nullptr));
        q = static_cast<std::uint8_t*>(run(q, inst, 0x800000001001ull, const_cast<std::uint8_t*>(stream), in_len, pcm.data(),
                                           pcm.size() * 2, run_side, 0x20, nullptr));
        if (start(ctx, buf, static_cast<std::uint64_t>(q - buf), 0x5a, err, &batch) != 0 || wait(ctx, batch, ~0u, err) != 0) return 1;
        // Frames inside a superframe vary in size: the ninth ends somewhere
        // inside the third superframe.
        const std::uint32_t used = rd32(run_side + 8);
        if ((rd32(run_side) & ~0x18u) != 0 || rd32(run_side + 12) != 2048 * sample_bytes || used <= 2 * superframe ||
            used >= 3 * superframe || rd32(run_side + 24) != 9) {
            std::fprintf(stderr, "seek: result %x consumed %u produced %u frames %u (want inside the third superframe, %u, 9)\n",
                         rd32(run_side), used, rd32(run_side + 12), rd32(run_side + 24), 2048 * sample_bytes);
            return 1;
        }
    }

    // FMOD's shared batch (0x1119d70): one run per channel, placed by the
    // caller 0x50 apart - the SDK's size of a run job with a return address.
    // Both channels must decode, not just the first.
    unsigned inst2 = 0;
    if (create(ctx, 1, 0x409 | (static_cast<std::uint64_t>(channels) << 3), &inst2) != 0) return 1;
    {
        std::uint8_t* q = buf;
        q = static_cast<std::uint8_t*>(control(q, inst2, 0x6001, init_param, 8, init_side, 8, nullptr));
        // Both channels start a stream here: the seek above left `inst` inside a superframe.
        q = static_cast<std::uint8_t*>(control(q, inst, 0x6001, init_param, 8, init_side, 8, nullptr));
        if (start(ctx, buf, static_cast<std::uint64_t>(q - buf), 0x5a, err, &batch) != 0 || wait(ctx, batch, ~0u, err) != 0) return 1;
    }
    std::vector<std::int16_t> pcm2(pcm.size());
    std::uint8_t side_a[0x20] = {}, side_b[0x20] = {};
    void* const ra = buf;  // any non-null return address
    std::memset(buf, 0, sizeof(buf));
    run(buf, inst, 0x800000001001ull, const_cast<std::uint8_t*>(stream), in_len, pcm.data(), pcm.size() * 2, side_a, 0x20, ra);
    run(buf + 0x50, inst2, 0x800000001001ull, const_cast<std::uint8_t*>(stream), in_len, pcm2.data(), pcm2.size() * 2, side_b, 0x20, ra);
    if (start(ctx, buf, 0xa0, 0x5a, err, &batch) != 0 || wait(ctx, batch, ~0u, err) != 0) return 1;
    if (rd32(side_a + 12) == 0 || rd32(side_b + 12) == 0) {
        std::fprintf(stderr, "shared batch: first run produced %u (result %x consumed %u), second %u - a job was skipped\n", rd32(side_a + 12), rd32(side_a), rd32(side_a + 8), rd32(side_b + 12));
        return 1;
    }
    std::printf("at9_test ok (mean |sample| %ld; gapless window, then a reused instance decodes in full; a shared batch runs "
                "every job)\n",
                energy / static_cast<long>(produced / 2));
    return 0;
}
