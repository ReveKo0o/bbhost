#include "core/portable.h"
#include "hle/common.h"
#include "hle/platform.h"
#include "hle/hle.h"
#include "host/audio.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {


struct AudioPort {
    std::uint32_t len = 256;
    std::uint32_t freq = 48000;
    std::uint32_t channels = 2;
    bool is_float = false;
    int host = 0;  // host audio handle, 0 = pacing only
    std::uint64_t next_us = 0;
};
std::mutex g_audio_mu;
std::unordered_map<int, AudioPort> g_audio;
int g_audio_next = 1;

GUEST_ABI int hle_audio_init() { return 0; }

// Microphone / voice chat: no capture device. Opens succeed with a handle,
// reads deliver silence-length zero so the callers do not spin.
GUEST_ABI int hle_audio_in_open(int, unsigned, int, unsigned, unsigned, unsigned) { return 1; }
GUEST_ABI int hle_audio_in_input(int, void*) { return 0; }
GUEST_ABI int hle_audio_in_close(int) { return 0; }
GUEST_ABI int hle_voice_ok() { return 0; }
GUEST_ABI int hle_voice_create_port(unsigned* id, const void*) {
    if (id) {
        *id = 1;
    }
    return 0;
}
GUEST_ABI int hle_voice_read(unsigned, void*, unsigned* size) {
    if (size) {
        *size = 0;
    }
    return 0;
}
GUEST_ABI int hle_voice_write(unsigned, const void*, unsigned* size) {
    if (size) {
        *size = 0;
    }
    return 0;
}
GUEST_ABI int hle_voice_port_info(unsigned, void* info) {
    if (info) {
        std::memset(info, 0, 32);
    }
    return 0;
}
// sceAudioOutOpen(userId, portType, index, len, freq, param). param low byte:
// 0 s16 mono, 1 s16 stereo, 2 s16 8ch, 3 f32 mono, 4 f32 stereo, 5 f32 8ch,
// 6 s16 8ch std, 7 f32 8ch std.
GUEST_ABI int hle_audio_open(int, int port_type, int, unsigned len, unsigned freq, unsigned param) {
    AudioPort p{};
    p.len = len ? len : 256;
    p.freq = freq ? freq : 48000;
    const unsigned fmt = param & 0xff;
    static const unsigned kChannels[8] = {1, 2, 8, 1, 2, 8, 8, 8};
    static const bool kFloat[8] = {false, false, false, true, true, true, false, true};
    p.channels = kChannels[fmt < 8 ? fmt : 1];
    p.is_float = kFloat[fmt < 8 ? fmt : 1];
    p.host = host_audio_open(p.freq, p.channels, p.is_float, p.len);
    std::lock_guard<std::mutex> lock(g_audio_mu);
    int h = g_audio_next++;
    g_audio[h] = p;
    host_log("sceAudioOutOpen type=%d len=%u freq=%u fmt=%u -> %d%s", port_type, p.len, p.freq, fmt, h,
             p.host ? "" : " (no host device)");
    return h;
}
GUEST_ABI int hle_audio_close(int h) {
    int host = 0;
    {
        std::lock_guard<std::mutex> lock(g_audio_mu);
        auto it = g_audio.find(h);
        if (it != g_audio.end()) {
            host = it->second.host;
            g_audio.erase(it);
        }
    }
    if (host) {
        host_audio_close(host);
    }
    return 0;
}
// sceAudioOutSetVolume(handle, flags, int32 vol[8]) with 0..32768 per channel.
GUEST_ABI int hle_audio_volume(int h, int flags, const int* vol) {
    int host = 0;
    unsigned channels = 2;
    {
        std::lock_guard<std::mutex> lock(g_audio_mu);
        auto it = g_audio.find(h);
        if (it == g_audio.end()) {
            return static_cast<int>(0x80260005);
        }
        host = it->second.host;
        channels = it->second.channels;
    }
    if (host && vol) {
        float sum = 0.0f;
        unsigned n = 0;
        for (unsigned c = 0; c < channels && c < 8; ++c) {
            if (flags & (1 << c)) {
                sum += static_cast<float>(vol[c]) / 32768.0f;
                ++n;
            }
        }
        if (n) {
            host_audio_set_gain(host, sum / static_cast<float>(n));
        }
    }
    return 0;
}
std::mutex g_audio_stats_mu;
std::map<int, std::uint64_t> g_audio_writes;  // by handle

GUEST_ABI int hle_audio_output(int handle, const void* data) {
    AudioPort p;
    {
        std::lock_guard<std::mutex> lock(g_audio_mu);
        auto it = g_audio.find(handle);
        if (it == g_audio.end()) {
            return static_cast<int>(0x80260005);
        }
        p = it->second;
    }
    if (data) {
        std::lock_guard<std::mutex> lock(g_audio_stats_mu);
        ++g_audio_writes[handle];
    }
    if (p.host && data) {
        host_audio_write(p.host, data, p.len);  // blocks to real-time
        return static_cast<int>(p.len);
    }
    if (!data) {
        return 0;  // NULL flushes; nothing queued here
    }
    const std::uint64_t dur = p.freq ? (static_cast<std::uint64_t>(p.len) * 1000000ull / p.freq) : 2000;
    const std::uint64_t now = now_us();
    if (p.next_us > now) {
        const std::uint64_t delay = p.next_us - now;
        if (delay < 100000) {
            host_sleep_us(delay);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_audio_mu);
        auto it = g_audio.find(handle);
        if (it != g_audio.end()) {
            it->second.next_us = now_us() + dur;
        }
    }
    return 0;
}
// SceAudioOutPortState { u16 output; u8 channel; u8 reserved; u16 volume; u16 rerouteCounter; u64 flag; u64 reserved[2] }
GUEST_ABI int hle_audio_state(int h, std::uint8_t* st) {
    if (!st) {
        return static_cast<int>(0x80260001);
    }
    std::memset(st, 0, 32);
    std::lock_guard<std::mutex> lock(g_audio_mu);
    auto it = g_audio.find(h);
    if (it == g_audio.end()) {
        return static_cast<int>(0x80260005);
    }
    st[0] = 1;  // SCE_AUDIO_OUT_STATE_OUTPUT_CONNECTED_PRIMARY
    st[2] = static_cast<std::uint8_t>(it->second.channels);
    const std::uint16_t volume = 32768 / 2;
    std::memcpy(st + 4, &volume, 2);
    return 0;
}

}  // namespace

std::string hle_audio_stats() {
    std::lock_guard<std::mutex> lock(g_audio_stats_mu);
    std::string out;
    for (const auto& kv : g_audio_writes) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " audio-port%d=%llu", kv.first, static_cast<unsigned long long>(kv.second));
        out += buf;
    }
    return out;
}

void hle_register_audio() {
#define REG(name, fn) register_hle_fn(name, reinterpret_cast<void*>(fn))
    REG("sceAudioOutInit", hle_audio_init);
    REG("sceAudioInOpen", hle_audio_in_open);
    REG("sceAudioInInput", hle_audio_in_input);
    REG("sceAudioInClose", hle_audio_in_close);
    REG("sceVoiceInit", hle_voice_ok);
    REG("sceVoiceEnd", hle_voice_ok);
    REG("sceVoiceStart", hle_voice_ok);
    REG("sceVoiceStop", hle_voice_ok);
    REG("sceVoiceCreatePort", hle_voice_create_port);
    REG("sceVoiceDeletePort", hle_voice_ok);
    REG("sceVoiceConnectIPortToOPort", hle_voice_ok);
    REG("sceVoiceDisconnectIPortFromOPort", hle_voice_ok);
    REG("sceVoiceGetPortInfo", hle_voice_port_info);
    REG("sceVoiceReadFromOPort", hle_voice_read);
    REG("sceVoiceWriteToIPort", hle_voice_write);
    REG("sceAudioOutOpen", hle_audio_open);
    REG("sceAudioOutClose", hle_audio_close);
    REG("sceAudioOutOutput", hle_audio_output);
    REG("sceAudioOutSetVolume", hle_audio_volume);
    REG("sceAudioOutGetPortState", hle_audio_state);
#undef REG
}
