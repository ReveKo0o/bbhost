// Host audio output lifetimes (src/host/audio.cpp) against SDL's dummy audio
// driver. A close or a shutdown while guest threads are inside
// host_audio_write must wait for those writes instead of destroying the
// stream under them (core 2044885), and later writes must return at once.
// Timing checks are loose on purpose: the machine may be busy.
#include "host/audio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

const std::vector<float> g_buffer(512 * 2, 0.25f);

// Writes 256-frame buffers until stopped, like a guest mixer thread.
struct Writer {
    std::atomic<bool> stop{false};
    std::atomic<long> writes{0};
    std::thread thread;

    void start(int handle) {
        thread = std::thread([this, handle] {
            while (!stop.load()) {
                host_audio_write(handle, g_buffer.data(), 256);
                writes.fetch_add(1);
            }
        });
    }
    void join() {
        stop = true;
        thread.join();
    }
};

// Both writers are running: each has finished a few writes (up to 2 s).
bool writers_running(const Writer& a, const Writer& b) {
    const auto t = Clock::now();
    while (ms_since(t) < 2000.0) {
        if (a.writes.load() >= 3 && b.writes.load() >= 3) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// 1,000 writes of 256 frames are over five seconds of audio; paced, they take
// that long. On a closed handle they return at once.
bool writes_return_at_once(int handle) {
    const auto t = Clock::now();
    for (int i = 0; i < 1000; ++i) host_audio_write(handle, g_buffer.data(), 256);
    return ms_since(t) < 500.0;
}

}  // namespace

int main() {
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);
    const int a = host_audio_open(48000, 2, true, 256);
    if (!a) {
        std::printf("host_audio_test: SDL dummy audio unavailable, skipped\n");
        return 77;
    }

    // Close while two writers are pacing to real time on the handle.
    {
        Writer w1, w2;
        w1.start(a);
        w2.start(a);
        check(writers_running(w1, w2), "both writers run before the close");
        const auto t = Clock::now();
        host_audio_close(a);
        check(ms_since(t) < 2000.0, "a close under two writers returns");
        check(writes_return_at_once(a), "writes on a closed handle return at once");
        w1.join();
        w2.join();
        host_audio_close(a);  // closing it again does nothing
    }

    // Shutdown while two outputs are written, racing a close of one of them.
    {
        const int b = host_audio_open(48000, 2, true, 256);
        const int c = host_audio_open(44100, 2, false, 512);
        check(b && c && b != c, "two outputs open");
        Writer wb, wc;
        wb.start(b);
        wc.start(c);
        check(writers_running(wb, wc), "both writers run before the shutdown");
        std::thread closer([c] { host_audio_close(c); });
        const auto t = Clock::now();
        host_audio_shutdown();
        closer.join();
        check(ms_since(t) < 2000.0, "a shutdown under writers returns");
        check(host_audio_open(48000, 2, true, 256) == 0, "no output opens after the shutdown");
        check(writes_return_at_once(b), "writes after the shutdown return at once");
        wb.join();
        wc.join();
        host_audio_shutdown();  // a second shutdown does nothing
    }

    std::printf("host_audio_test: %s\n", g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
