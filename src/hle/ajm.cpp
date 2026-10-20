// sceAjm: the console's audio codec unit. FMOD in the eboot decodes ATRAC9
// through it. Jobs are recorded into the caller's batch buffer and run on
// sceAjmBatchStartBuffer; ATRAC9 decoding is LibAtrac9 (MIT, third_party).
#include "hle/common.h"
#include "host/frame_stats.h"
#include "hle/hle.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "libatrac9.h"
}

namespace {

constexpr int kAjmInvalid = static_cast<int>(0x80930004);
constexpr int kAjmInProgress = static_cast<int>(0x8093000f);
constexpr int kAjmCodecAt9 = 1;

// SceAjmSidebandResult iResult bits. FMOD's AT9 codec (0x1119c00) takes a run
// as good only when nothing but PARTIAL_INPUT (0x8) / NOT_ENOUGH_ROOM (0x10)
// is set; anything else ends the sound.
constexpr int kResultOk = 0;
constexpr int kResultNotInitialized = 0x1;
constexpr int kResultInvalidData = 0x2;
constexpr int kResultPartialInput = 0x8;    // the next frame's superframe is not all there
constexpr int kResultNotEnoughRoom = 0x10;  // the next frame's samples do not fit
// Instance flag (sceAjmInstanceCreate): at the end of the gapless window,
// restart it (skip again, count again) and go on decoding. FMOD sets it on
// every instance and loops a sound by feeding the loop start after its end.
constexpr std::uint64_t kInstanceGaplessLoop = 1ull << 10;

// Flag layout (SDK ajm.h): 3 revision bits, 8 codec bits, then the run/control
// bits; sideband bits are fixed at 45..47. FMOD's AT9 codec in the eboot uses
// control 0x6001 (init+reset), 0x200000002001 (gapless+reset), run 0x801
// (codec info) and 0x800000001001 (stream+multiple frames).
constexpr std::uint64_t kSideGapless = 1ull << 45;
constexpr std::uint64_t kSideFormat = 1ull << 46;
constexpr std::uint64_t kSideStream = 1ull << 47;
constexpr std::uint64_t kRunGetCodecInfo = 1ull << 11;
constexpr std::uint64_t kRunMultipleFrames = 1ull << 12;
constexpr std::uint64_t kControlReset = 1ull << 13;
constexpr std::uint64_t kControlInitialize = 1ull << 14;

struct At9State {
    void* handle = nullptr;
    bool ready = false;
    Atrac9CodecInfo info{};
    std::uint64_t total_samples = 0;
    // SceAjmSidebandGaplessDecode: samples to emit in total (0 = unlimited),
    // encoder-delay samples to drop first, and how many were dropped so far.
    std::uint32_t gapless_total = 0;
    std::uint16_t gapless_skip = 0;
    std::uint16_t gapless_skipped = 0;
    std::uint64_t emitted = 0;  // samples per channel emitted since reset
    // Where decoding is inside the current superframe: its bytes not yet
    // consumed and its frames decoded. Kept across runs - the unit decodes
    // frame by frame, and a run may end mid-superframe.
    std::size_t sf_remain = 0;
    std::uint32_t sf_frame = 0;
    std::vector<std::int16_t> frame;  // one decoded frame
    std::FILE* dump = nullptr;        // BBHOST_AJM_DUMP: this stream's decoded output
    std::string dump_path;            // and where its first input bytes go (.head)
    bool head_written = true;
    bool sig_pending = false;         // BBHOST_AJM_STARTS: the next run names the stream's sample
    ~At9State() {
        if (handle) {
            Atrac9ReleaseHandle(handle);
        }
        if (dump) {
            std::fclose(dump);
        }
    }
};

// A decoder that starts clean: LibAtrac9's InitDecoder reads the config but
// keeps the frame's position in its superframe, the previous frame's IMDCT
// overlap and scale factors. Re-initializing a handle that stopped inside a
// superframe then failed the next stream's first frame, and every pooled
// instance mixed the last sound's tail into the next one's start. A new
// (zeroed) handle each time, as a reset on the unit is.
int fresh_decoder(At9State& st, unsigned char* config) {
    if (st.handle) Atrac9ReleaseHandle(st.handle);
    st.handle = Atrac9GetHandle();
    return Atrac9InitDecoder(st.handle, config);
}

struct AjmInst {
    int codec = 0;
    std::uint64_t flags = 0;
    std::shared_ptr<At9State> at9;
};
struct AjmBatch {
    bool done = true;
    bool cancelled = false;
};
struct AjmCtx {
    std::unordered_set<int> codecs;
    std::unordered_map<int, AjmInst> inst;
    std::unordered_map<int, AjmBatch> batches;
    int next_inst = 1;
    int next_batch = 1;
};
std::mutex g_ajm_mu;
int g_ajm_next = 1;
std::unordered_map<int, AjmCtx> g_ajm;

AjmCtx* ajm_ctx(unsigned id) {
    auto it = g_ajm.find(static_cast<int>(id));
    return it == g_ajm.end() ? nullptr : &it->second;
}

// Records written into the guest's batch buffer, each in the space the SDK's
// job takes. A job is chunks - an 8-byte job header, then a 16-byte return
// address (when one is passed), a 16-byte input buffer, 8 bytes of flags, a
// 16-byte output buffer and, for a run, a 16-byte sideband buffer: a control
// job is 0x30 bytes, 0x40 with a return address, a run 0x40 / 0x50.
//
// Callers place jobs at those offsets themselves: FMOD's AT9 codec puts the
// runs of every channel it decodes into one shared batch at 0x50 apart
// (0x1119d70). The run was advanced 0x90 here, so the parser found the second
// run's padding where it expected a job and stopped: every channel but the
// first in a shared batch got nothing decoded, FMOD read "0 samples decoded"
// as the end of the sound, and 69 of 84 sounds in a minute of play stopped
// after their first job or two. It also skipped the gapless control of
// FMOD's seek batch (control, codec-info run, gapless control, run).
constexpr std::uint32_t kMagicControl = 0x4c544341u;    // 'ACTL', 0x30
constexpr std::uint32_t kMagicControlRa = 0x52544341u;  // 'ACTR', 0x40
constexpr std::uint32_t kMagicRun = 0x4e555241u;        // 'ARUN', 0x40
constexpr std::uint32_t kMagicRunRa = 0x52555241u;      // 'AURR', 0x50
struct ControlRec {
    std::uint32_t magic;
    std::uint32_t inst;
    std::uint64_t flags;
    void* side_in;
    std::uint64_t side_in_sz;
    void* side_out;
    std::uint64_t side_out_sz;
};
struct RunRec {
    std::uint32_t magic;
    std::uint32_t inst;
    std::uint64_t flags;
    void* in;
    std::uint64_t in_sz;
    void* out;
    std::uint64_t out_sz;
    void* side_out;
    std::uint64_t side_out_sz;
};
static_assert(sizeof(ControlRec) == 0x30 && sizeof(RunRec) == 0x40, "AJM job records fit the smallest jobs");
std::size_t job_size(std::uint32_t magic) {
    switch (magic) {
    case kMagicControl: return 0x30;
    case kMagicControlRa: return 0x40;
    case kMagicRun: return 0x40;
    case kMagicRunRa: return 0x50;
    default: return 0;
    }
}

// ---- sideband writer
struct Sideband {
    std::uint8_t* p;
    std::size_t cap;
    std::size_t off = 0;
    void put(const void* src, std::size_t n) {
        if (p && off + n <= cap) {
            std::memcpy(p + off, src, n);
        }
        off += n;
    }
};

// What runs did (BBHOST_AJM_STATS=1 logs every 5 s; a window with anything
// odd always logs): runs, runs that produced nothing from a superframe of
// input, runs at their gapless end, invalid data.
struct AjmStats {
    std::uint64_t runs = 0, empty_runs = 0, gapless_ends = 0, bad = 0;
    // What FMOD's AT9 codec (0x1119c00) reads as the end of a sound: a run
    // result with bits other than PARTIAL_INPUT / NOT_ENOUGH_ROOM (an error:
    // the sound stops), or total_decoded_samples 0 (decoding finished).
    std::uint64_t fmod_errors = 0, fmod_total_zero = 0, control_errors = 0;
    std::uint64_t window_ms = 0;
} g_ajm_stats;

std::uint64_t ajm_now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// BBHOST_AJM_TRACE=<n>: log the first n control and run jobs in full.
int ajm_trace_left() {
    static std::atomic<int> left{[] { const char* e = std::getenv("BBHOST_AJM_TRACE"); return e ? std::atoi(e) : 0; }()};
    return left.fetch_sub(1) > 0;
}

void exec_control(AjmCtx& c, const ControlRec& r) {
    if (ajm_trace_left()) {
        std::uint32_t in[4] = {};
        if (r.side_in) std::memcpy(in, r.side_in, std::min<std::size_t>(sizeof(in), static_cast<std::size_t>(r.side_in_sz)));
        host_log("ajm trace: t=%llu control inst=%u flags=0x%llx side_in %llu bytes [%08x %08x %08x %08x] side_out %llu",
                 static_cast<unsigned long long>(ajm_now_ms()), r.inst, static_cast<unsigned long long>(r.flags), static_cast<unsigned long long>(r.side_in_sz), in[0], in[1], in[2], in[3],
                 static_cast<unsigned long long>(r.side_out_sz));
    }
    std::int32_t result[2] = {kResultOk, 0};
    auto it = c.inst.find(static_cast<int>(r.inst));
    if (it == c.inst.end()) {
        result[0] = kResultInvalidData;
    } else {
        AjmInst& inst = it->second;
        if (inst.codec == kAjmCodecAt9) {
            if (!inst.at9) {
                inst.at9 = std::make_shared<At9State>();
                inst.at9->handle = Atrac9GetHandle();
            }
            // Input sidebands in the SDK's order: format, gapless, then the
            // initialize parameters.
            const auto* side_in = static_cast<const std::uint8_t*>(r.side_in);
            std::size_t side_off = 0;
            if (r.flags & kSideFormat) side_off += 24;
            const std::size_t gapless_off = side_off;
            if (r.flags & kSideGapless) side_off += 8;
            At9State& st = *inst.at9;
            // A reset starts a new stream: the gapless window and its counters
            // go with the old one. They were kept, and FMOD reuses instances
            // from a pool: a sound started at an offset (its seek sets a
            // gapless total, the samples it has left) handed its instance to
            // the next sound with that total still in force - the next sound
            // decoded nothing, or stopped where the old one would have. Small
            // sound effects went missing or were cut off. FMOD sends a new
            // gapless total after the reset whenever it wants one.
            if (r.flags & (kControlReset | kControlInitialize)) {
                // BBHOST_AJM_STARTS=1: a line per stream an instance starts -
                // one per sound FMOD plays from a compressed sample, so a count
                // over a stretch of play says which sounds were started.
                static const bool starts = [] {
                    const char* e = std::getenv("BBHOST_AJM_STARTS");
                    return e && e[0] == '1';
                }();
                if (starts) {
                    host_log("ajm: start t=%llu inst=%u %s", static_cast<unsigned long long>(ajm_now_ms()), r.inst,
                             (r.flags & kControlInitialize) ? "initialize" : "reset");
                    st.sig_pending = true;
                }
                // BBHOST_AJM_DUMP=<dir>: each stream an instance decodes (from
                // one reset to the next) as <dir>/ajm-<n>-inst<i>-<ch>ch.raw,
                // s16 interleaved - what FMOD got, to compare with what played.
                if (const char* dir = std::getenv("BBHOST_AJM_DUMP")) {
                    static std::atomic<int> seq{0};
                    if (st.dump) std::fclose(st.dump);
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s/ajm-%05d-inst%u", dir, seq.fetch_add(1), r.inst);
                    st.dump_path = path;
                    st.dump = std::fopen((st.dump_path + ".raw").c_str(), "wb");
                    st.head_written = false;
                }
                st.total_samples = 0;
                st.emitted = 0;
                st.gapless_total = 0;
                st.gapless_skip = 0;
                st.gapless_skipped = 0;
                st.sf_remain = static_cast<std::size_t>(st.info.superframeSize);
                st.sf_frame = 0;
            }
            if ((r.flags & kControlInitialize) && side_in && r.side_in_sz >= side_off + 4) {
                unsigned char cfg[4];
                std::memcpy(cfg, side_in + side_off, 4);
                int rc = fresh_decoder(*inst.at9, cfg);
                if (rc == 0) {
                    Atrac9GetCodecInfo(inst.at9->handle, &inst.at9->info);
                    inst.at9->ready = true;
                    inst.at9->total_samples = 0;
                    inst.at9->sf_remain = static_cast<std::size_t>(inst.at9->info.superframeSize);
                    inst.at9->sf_frame = 0;
                    inst.at9->frame.assign(static_cast<std::size_t>(inst.at9->info.frameSamples) *
                                               static_cast<std::size_t>(inst.at9->info.channels), 0);
                    static std::atomic<int> logs{0};
                    if (logs.fetch_add(1) < 4) {
                        host_log("ajm at9 init inst=%u: %d ch %d Hz, %d frames/superframe of %d samples, superframe %d bytes",
                                 r.inst, inst.at9->info.channels, inst.at9->info.samplingRate,
                                 inst.at9->info.framesInSuperframe, inst.at9->info.frameSamples,
                                 inst.at9->info.superframeSize);
                    }
                } else {
                    host_log("ajm at9 init failed rc=%d cfg=%02x%02x%02x%02x", rc, cfg[0], cfg[1], cfg[2], cfg[3]);
                    result[0] = kResultInvalidData;
                }
            } else if ((r.flags & kControlReset) && st.ready) {
                fresh_decoder(st, st.info.configData);
            }
            if ((r.flags & kSideGapless) && side_in && r.side_in_sz >= gapless_off + 8) {
                std::memcpy(&st.gapless_total, side_in + gapless_off, 4);
                std::memcpy(&st.gapless_skip, side_in + gapless_off + 4, 2);
            }
        }
    }
    if (result[0] != 0) ++g_ajm_stats.control_errors;
    Sideband sb{static_cast<std::uint8_t*>(r.side_out), static_cast<std::size_t>(r.side_out_sz)};
    if (sb.p && sb.cap) {
        std::memset(sb.p, 0, sb.cap);
    }
    sb.put(result, 8);
    if ((r.flags & kSideGapless) && it != c.inst.end() && it->second.at9) {
        const At9State& st = *it->second.at9;
        std::uint32_t g[2] = {st.gapless_total,
                              static_cast<std::uint32_t>(st.gapless_skip) | (static_cast<std::uint32_t>(st.gapless_skipped) << 16)};
        sb.put(g, 8);
    }
}

void exec_run(AjmCtx& c, const RunRec& r) {
    std::int32_t result[2] = {kResultOk, 0};
    std::int32_t consumed = 0, produced = 0;
    std::uint32_t frames = 0;
    std::uint64_t total = 0;
    auto it = c.inst.find(static_cast<int>(r.inst));
    AjmInst* inst = it == c.inst.end() ? nullptr : &it->second;
    if (!inst) {
        result[0] = kResultInvalidData;
    } else if (inst->codec != kAjmCodecAt9 || !inst->at9 || !inst->at9->ready) {
        result[0] = kResultNotInitialized;
    } else {
        At9State& st = *inst->at9;
        const std::size_t ch = static_cast<std::size_t>(st.info.channels);
        const std::size_t frame_samples = static_cast<std::size_t>(st.info.frameSamples);
        const bool multi = (r.flags & kRunMultipleFrames) != 0;
        const std::uint8_t* in = static_cast<const std::uint8_t*>(r.in);
        if (st.sig_pending && in && r.in_sz) {
            // The stream's first input bytes name the sample it plays (a
            // sound played twice has the same ones): FNV-1a of up to 64.
            std::uint32_t h = 2166136261u;
            for (std::size_t k = 0; k < std::min<std::size_t>(64, static_cast<std::size_t>(r.in_sz)); ++k) h = (h ^ in[k]) * 16777619u;
            host_log("ajm: sound t=%llu inst=%u sig=%08x %dch", static_cast<unsigned long long>(ajm_now_ms()), r.inst, h,
                     st.info.channels);
            st.sig_pending = false;
        }
        std::uint8_t* out = static_cast<std::uint8_t*>(r.out);
        const std::size_t in_sz = static_cast<std::size_t>(r.in_sz);
        const std::size_t out_sz = static_cast<std::size_t>(r.out_sz);
        // Frame by frame, as the unit decodes: input is consumed a frame at a
        // time (the padding at a superframe's end with its last frame), a frame
        // is decoded only when the rest of its superframe is in the input, and
        // a run stops - PARTIAL_INPUT / NOT_ENOUGH_ROOM - when the next frame's
        // input is not all there or its samples would not fit. Whole
        // superframes only (what this did before) fell a frame short of what
        // FMOD plans for: its seek asks for its whole 2048-sample ring from
        // three superframes after a 256-sample skip - nine frames - and got
        // eight, which left its ring and its reads 256 samples out of step.
        const std::uint32_t sf_frames = static_cast<std::uint32_t>(st.info.framesInSuperframe);
        const std::size_t sf_bytes = static_cast<std::size_t>(st.info.superframeSize);
        const bool loop = (inst->flags & kInstanceGaplessLoop) != 0;
        const auto gapless_end = [&] { return st.gapless_total != 0 && st.emitted >= st.gapless_total; };
        // The stream's first input bytes, to find the sound in its FSB5 bank.
        if (!st.head_written && in && in_sz >= 64) {
            st.head_written = true;
            if (std::FILE* h = std::fopen((st.dump_path + ".head").c_str(), "wb")) {
                std::fwrite(in, 1, 64, h);
                std::fclose(h);
            }
        }
        const auto decode_frame = [&](int& used) {
            used = 0;
            const int rc = Atrac9Decode(st.handle, in + consumed, st.frame.data(), &used);
            if (rc != 0 || used <= 0 || static_cast<std::size_t>(used) > st.sf_remain) return false;
            consumed += used;
            st.sf_remain -= static_cast<std::size_t>(used);
            ++st.sf_frame;
            ++frames;
            st.total_samples += frame_samples;
            return true;
        };
        const auto end_superframe = [&] {
            consumed += static_cast<std::int32_t>(st.sf_remain);  // its padding
            st.sf_remain = sf_bytes;
            st.sf_frame = 0;
        };
        while (in && out) {
            if (loop && gapless_end()) {
                st.emitted = 0;
                st.gapless_skipped = 0;
            }
            if (gapless_end()) break;  // nothing more to emit
            const std::size_t skip_now = std::min<std::size_t>(
                frame_samples, st.gapless_skip > st.gapless_skipped ? static_cast<std::size_t>(st.gapless_skip - st.gapless_skipped) : 0);
            std::size_t count = frame_samples - skip_now;
            if (st.gapless_total != 0) count = std::min<std::size_t>(count, static_cast<std::size_t>(st.gapless_total - st.emitted));
            if (static_cast<std::size_t>(produced) + count * ch * 2 > out_sz) {
                result[0] |= kResultNotEnoughRoom;
                break;
            }
            if (static_cast<std::size_t>(consumed) + st.sf_remain > in_sz) {
                result[0] |= kResultPartialInput;
                break;
            }
            int used = 0;
            if (!decode_frame(used)) {
                // Resync on a fresh superframe; the overlap window restarts.
                fresh_decoder(st, st.info.configData);
                st.sf_remain = sf_bytes;
                st.sf_frame = 0;
                result[0] = kResultInvalidData;
                break;
            }
            st.gapless_skipped = static_cast<std::uint16_t>(st.gapless_skipped + skip_now);
            if (count) {
                if (st.dump) std::fwrite(st.frame.data() + skip_now * ch, 2, count * ch, st.dump);
                std::memcpy(out + produced, st.frame.data() + skip_now * ch, count * ch * 2);
                produced += static_cast<std::int32_t>(count * ch * 2);
                st.emitted += count;
            }
            if (st.sf_frame == sf_frames) {
                end_superframe();
            } else if (gapless_end()) {
                // The window ended inside a superframe: decode its other frames
                // for nothing, so the next input starts on a superframe.
                bool ok = true;
                while (ok && st.sf_frame < sf_frames) ok = decode_frame(used);
                end_superframe();
            }
            if (!multi) break;
        }
        total = st.total_samples;
        // The accounting above (AjmStats).
        ++g_ajm_stats.runs;
        if (gapless_end()) ++g_ajm_stats.gapless_ends;
        if (result[0] & kResultInvalidData) ++g_ajm_stats.bad;
        if (produced == 0 && in_sz >= sf_bytes && !gapless_end()) {
            ++g_ajm_stats.empty_runs;
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 12) {
                host_log("ajm: run inst=%u %zu ch in=%zu out=%zu produced nothing (result %x, superframe %zu/%zu bytes left, frame %u)",
                         r.inst, ch, in_sz, out_sz, result[0], st.sf_remain, sf_bytes, st.sf_frame);
            }
        }
    }
    {
        static const bool always = [] { const char* e = std::getenv("BBHOST_AJM_STATS"); return e && *e == '1'; }();
        const std::uint64_t now = ajm_now_ms();
        if (!g_ajm_stats.window_ms) g_ajm_stats.window_ms = now;
        if (now - g_ajm_stats.window_ms >= 5000) {
            if (always || g_ajm_stats.empty_runs || g_ajm_stats.bad || g_ajm_stats.fmod_errors || g_ajm_stats.fmod_total_zero ||
                g_ajm_stats.control_errors) {
                host_log("ajm: %llu runs, %llu produced nothing, %llu at their gapless end, %llu bad; FMOD stops: "
                         "%llu error results, %llu zero totals, %llu failed controls",
                         static_cast<unsigned long long>(g_ajm_stats.runs),
                         static_cast<unsigned long long>(g_ajm_stats.empty_runs),
                         static_cast<unsigned long long>(g_ajm_stats.gapless_ends), static_cast<unsigned long long>(g_ajm_stats.bad),
                         static_cast<unsigned long long>(g_ajm_stats.fmod_errors),
                         static_cast<unsigned long long>(g_ajm_stats.fmod_total_zero),
                         static_cast<unsigned long long>(g_ajm_stats.control_errors));
            }
            g_ajm_stats = AjmStats{};
            g_ajm_stats.window_ms = now;
        }
    }
    if (result[0] & ~0x18) ++g_ajm_stats.fmod_errors;
    if ((r.flags & kSideStream) && total == 0) {
        ++g_ajm_stats.fmod_total_zero;
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 12) {
            host_log("ajm: run inst=%u in=%llu out=%llu reports 0 samples decoded since its reset (result %d) - FMOD stops the sound",
                     r.inst, static_cast<unsigned long long>(r.in_sz), static_cast<unsigned long long>(r.out_sz), result[0]);
        }
    }
    Sideband sb{static_cast<std::uint8_t*>(r.side_out), static_cast<std::size_t>(r.side_out_sz)};
    if (sb.p && sb.cap) {
        std::memset(sb.p, 0, sb.cap);
    }
    sb.put(result, 8);
    if (r.flags & kSideStream) {
        std::int32_t s[2] = {consumed, produced};
        sb.put(s, 8);
        sb.put(&total, 8);
    }
    if (r.flags & kSideFormat) {
        std::uint32_t fmt[6] = {0, 0, 0, 0, 0, 0};
        if (inst && inst->at9 && inst->at9->ready) {
            fmt[0] = static_cast<std::uint32_t>(inst->at9->info.channels);
            fmt[1] = inst->at9->info.channels == 1 ? 0x4u : 0x3u;  // SCE_AJM_CHANNELMASK_ mono/stereo
            fmt[2] = static_cast<std::uint32_t>(inst->at9->info.samplingRate);
            fmt[3] = 0;  // SCE_AJM_FORMAT_ENCODING_S16
        }
        sb.put(fmt, 24);
    }
    if (r.flags & kSideGapless) {
        std::uint32_t g[2] = {0, 0};
        if (inst && inst->at9) {
            g[0] = inst->at9->gapless_total;
            g[1] = static_cast<std::uint32_t>(inst->at9->gapless_skip) |
                   (static_cast<std::uint32_t>(inst->at9->gapless_skipped) << 16);
        }
        sb.put(g, 8);
    }
    if (r.flags & kRunGetCodecInfo) {
        // SceAjmDecAt9CodecInfoSideband { uiSuperFrameSize, uiFramesInSuperFrame, uiFrameSamples, uiNextInputSize }
        std::uint32_t ci[4] = {0, 0, 0, 0};
        if (inst && inst->at9 && inst->at9->ready) {
            ci[0] = static_cast<std::uint32_t>(inst->at9->info.superframeSize);
            ci[1] = static_cast<std::uint32_t>(inst->at9->info.framesInSuperframe);
            ci[2] = static_cast<std::uint32_t>(inst->at9->info.frameSamples);
            ci[3] = static_cast<std::uint32_t>(inst->at9->info.superframeSize);
        }
        sb.put(ci, 16);
    }
    if (r.flags & kRunMultipleFrames) {
        std::uint32_t mf[2] = {frames, 0};
        sb.put(mf, 8);
    }
    static std::atomic<int> logs{0};
    if (ajm_trace_left()) {
        host_log("ajm trace: t=%llu run inst=%u ch %d flags=0x%llx in=%llu out=%llu -> consumed=%d produced=%d frames=%u result=%d "
                 "total=%llu gapless %u/%u emitted %llu",
                 static_cast<unsigned long long>(ajm_now_ms()), r.inst, inst && inst->at9 ? inst->at9->info.channels : 0, static_cast<unsigned long long>(r.flags),
                 static_cast<unsigned long long>(r.in_sz), static_cast<unsigned long long>(r.out_sz), consumed, produced, frames,
                 result[0], static_cast<unsigned long long>(total), inst && inst->at9 ? inst->at9->gapless_total : 0,
                 inst && inst->at9 ? inst->at9->gapless_skip : 0,
                 static_cast<unsigned long long>(inst && inst->at9 ? inst->at9->emitted : 0));
    } else if (logs.fetch_add(1) < 6) {
        host_log("ajm at9 run inst=%u flags=0x%llx in=%llu out=%llu -> consumed=%d produced=%d frames=%u result=%d", r.inst,
                 static_cast<unsigned long long>(r.flags), static_cast<unsigned long long>(r.in_sz),
                 static_cast<unsigned long long>(r.out_sz), consumed, produced, frames, result[0]);
    }
}

// ---- entry points
GUEST_ABI int hle_ajm_init(std::int64_t, unsigned* ctx) {
    if (!ctx) {
        return kAjmInvalid;
    }
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    const int id = g_ajm_next++;
    g_ajm[id] = AjmCtx{};
    *ctx = static_cast<unsigned>(id);
    host_log("sceAjmInitialize -> %d", id);
    return 0;
}
GUEST_ABI int hle_ajm_fini(unsigned ctx) {
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    g_ajm.erase(static_cast<int>(ctx));
    return 0;
}
GUEST_ABI int hle_ajm_mod_reg(unsigned ctx, int codec, std::int64_t) {
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    AjmCtx* c = ajm_ctx(ctx);
    if (!c) {
        return kAjmInvalid;
    }
    c->codecs.insert(codec);
    host_log("sceAjmModuleRegister ctx=%u codec=%d%s", ctx, codec, codec == kAjmCodecAt9 ? " (ATRAC9)" : "");
    return 0;
}
GUEST_ABI int hle_ajm_mod_unreg(unsigned ctx, int codec) {
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    AjmCtx* c = ajm_ctx(ctx);
    if (c) {
        c->codecs.erase(codec);
    }
    return 0;
}
GUEST_ABI int hle_ajm_inst_create(unsigned ctx, int codec, std::uint64_t flags, unsigned* inst) {
    if (!inst) {
        return kAjmInvalid;
    }
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    AjmCtx* c = ajm_ctx(ctx);
    if (!c) {
        return kAjmInvalid;
    }
    const int id = c->next_inst++;
    AjmInst i;
    i.codec = codec;
    i.flags = flags;
    // FMOD's codec passes 0x409/0x411/0x421/0x431/0x441: revision 1, channel
    // count << 3, and a constant 0x400; it sizes its output buffers for s16.
    c->inst[id] = i;
    *inst = static_cast<unsigned>(id);
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 4) {
        host_log("sceAjmInstanceCreate ctx=%u codec=%d flags=0x%llx -> %d", ctx, codec,
                 static_cast<unsigned long long>(flags), id);
    }
    return 0;
}
GUEST_ABI int hle_ajm_inst_destroy(unsigned ctx, unsigned inst) {
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    AjmCtx* c = ajm_ctx(ctx);
    if (c) {
        c->inst.erase(static_cast<int>(inst));
    }
    return 0;
}
GUEST_ABI void* hle_ajm_job_control(void* batch, unsigned inst, std::uint64_t flags, void* side_in, std::uint64_t side_in_sz,
                                   void* side_out, std::uint64_t side_out_sz, void* ra) {
    if (!batch) {
        return nullptr;
    }
    ControlRec r{ra ? kMagicControlRa : kMagicControl, inst, flags, side_in, side_in_sz, side_out, side_out_sz};
    std::memcpy(batch, &r, sizeof(r));
    return static_cast<std::uint8_t*>(batch) + job_size(r.magic);
}
GUEST_ABI void* hle_ajm_job_run(void* batch, unsigned inst, std::uint64_t flags, void* in, std::uint64_t in_sz, void* out,
                               std::uint64_t out_sz, void* side_out, std::uint64_t side_out_sz, void* ra) {
    if (!batch) {
        return nullptr;
    }
    RunRec r{ra ? kMagicRunRa : kMagicRun, inst, flags, in, in_sz, out, out_sz, side_out, side_out_sz};
    std::memcpy(batch, &r, sizeof(r));
    return static_cast<std::uint8_t*>(batch) + job_size(r.magic);
}
// int sceAjmBatchStartBuffer(ctx, const void* batch, size_t size, int priority, SceAjmBatchError* err, SceAjmBatchId* out)
GUEST_ABI int hle_ajm_batch_start(unsigned ctx, void* batch, std::uint64_t size, int, void*, unsigned* out_id) {
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    AjmCtx* c = ajm_ctx(ctx);
    if (!c) {
        return kAjmInvalid;
    }
    const auto* p = static_cast<const std::uint8_t*>(batch);
    std::size_t off = 0;
    while (p && off + 8 <= size) {
        std::uint32_t magic;
        std::memcpy(&magic, p + off, 4);
        const std::size_t advance = job_size(magic);
        if ((magic == kMagicControl || magic == kMagicControlRa) && off + sizeof(ControlRec) <= size) {
            ControlRec r;
            std::memcpy(&r, p + off, sizeof(r));
            exec_control(*c, r);
        } else if ((magic == kMagicRun || magic == kMagicRunRa) && off + sizeof(RunRec) <= size) {
            RunRec r;
            std::memcpy(&r, p + off, sizeof(r));
            exec_run(*c, r);
        } else {
            // Not a job this module wrote: say so - the rest of the batch
            // would silently go undone.
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 8) {
                host_log("ajm: batch %p has no job at +0x%zx of 0x%llx bytes (word %08x) - the rest is skipped", batch, off,
                         static_cast<unsigned long long>(size), magic);
            }
            break;
        }
        off += advance;
    }
    const int id = c->next_batch++;
    c->batches[id] = AjmBatch{true, false};
    if (out_id) {
        *out_id = static_cast<unsigned>(id);
    }
    return 0;
}
GUEST_ABI int hle_ajm_batch_wait(unsigned ctx, unsigned batch, unsigned timeout, void* err) {
    MainThreadWait timed(0);
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    AjmCtx* c = ajm_ctx(ctx);
    if (!c) {
        return kAjmInvalid;
    }
    auto it = c->batches.find(static_cast<int>(batch));
    if (it == c->batches.end()) {
        return kAjmInvalid;
    }
    if (!it->second.done && timeout == 0) {
        return kAjmInProgress;
    }
    if (err) {
        std::memset(err, 0, 32);
    }
    const bool cancelled = it->second.cancelled;
    c->batches.erase(it);
    return cancelled ? static_cast<int>(0x80930017) : 0;
}
GUEST_ABI int hle_ajm_batch_cancel(unsigned ctx, unsigned batch) {
    std::lock_guard<std::mutex> lock(g_ajm_mu);
    AjmCtx* c = ajm_ctx(ctx);
    if (!c) {
        return kAjmInvalid;
    }
    auto it = c->batches.find(static_cast<int>(batch));
    if (it != c->batches.end()) {
        it->second.cancelled = true;
    }
    return 0;
}
GUEST_ABI int hle_ajm_batch_error_dump(void*, void* err) {
    if (err) {
        std::memset(err, 0, 32);
    }
    return 0;
}

}  // namespace

void hle_register_ajm() {
#define REG(name, fn) register_hle_fn(name, reinterpret_cast<void*>(fn))
    REG("sceAjmInitialize", hle_ajm_init);
    REG("sceAjmFinalize", hle_ajm_fini);
    REG("sceAjmModuleRegister", hle_ajm_mod_reg);
    REG("sceAjmModuleUnregister", hle_ajm_mod_unreg);
    REG("sceAjmInstanceCreate", hle_ajm_inst_create);
    REG("sceAjmInstanceDestroy", hle_ajm_inst_destroy);
    REG("sceAjmBatchJobControlBufferRa", hle_ajm_job_control);
    REG("sceAjmBatchJobRunBufferRa", hle_ajm_job_run);
    REG("sceAjmBatchStartBuffer", hle_ajm_batch_start);
    REG("sceAjmBatchWait", hle_ajm_batch_wait);
    REG("sceAjmBatchCancel", hle_ajm_batch_cancel);
    REG("sceAjmBatchErrorDump", hle_ajm_batch_error_dump);
#undef REG
}
