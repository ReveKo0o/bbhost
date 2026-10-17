// "Compile shaders now" in the setup window (host/launcher.h).
//
// The game compiles its shaders itself - at the start, and DXC's modules
// behind the game for the next start (host/translation_cache.h) - and a player
// who skips this loses nothing. The button does all of it up front: it starts
// bbhost again as a child process, `bbhost --compile-shaders` with the same
// arguments, which starts the GPU device with no game and compiles every
// shader the game's bundles hold (the debug menu's aside) and every stage and
// pipeline the manifests name - translations, lifts, DXC's modules and the
// driver's pipelines - into the same caches, on the same precompile workers
// the game uses (host_gpu_set_compile_session). The child prints its
// progress on stdout ("compile-shaders: ..." lines), logs to
// <data>/bbhost/shader-compile.log, ends with the caches written, and on
// "cancel" (or its stdin closing) drops what it has not begun and writes what
// it has. What it did is kept in <data>/bbhost/shader-compile.txt.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// main.cpp, before the configuration is read: whether the arguments ask for
// the compile session (the flag is taken out of them), and the arguments the
// setup window starts its child with.
bool shader_compile_take_flag(int& argc, char** argv);
void shader_compile_set_args(int argc, char** argv);
// The child's run, once the game's folders are known; its exit code.
int shader_compile_session();

// What the setup window shows before anything runs.
struct ShaderCacheState {
    std::string dir;                // <data>/bbhost
    bool translator_current = false;  // shader-cache-build.txt names this bbhost's translator
    bool have_caches = false;
    std::string cache_translator;   // the one the caches were made by ("" none)
    int device_current = -1;        // the pipeline cache's device among this machine's: 1 yes, 0 no, -1 not known (yet)
    std::string cache_device;       // "vendor:device uuid" from the pipeline cache's header
    std::uint64_t translation_bytes = 0, pipeline_bytes = 0, stage_manifest_bytes = 0, pipeline_manifest_bytes = 0;
    // shader-compile.txt: the last compile session's result.
    std::vector<std::string> last;  // its lines
};
// `data` is the data folder the game will use ("" = the default). The device
// check enumerates this machine's Vulkan devices: done on a thread of its own
// by ShaderCompileJob::refresh.
ShaderCacheState shader_cache_state(const std::string& data, const std::vector<std::string>& devices);
// This machine's Vulkan devices, "vendor:device uuid" each (pipelineCacheUUID).
std::vector<std::string> shader_vulkan_devices();

// The child, from the setup window.
class ShaderCompileJob {
public:
    ~ShaderCompileJob();
    bool start(std::string& error);
    void cancel();
    bool running() const;
    // Under the job's own lock, what the child said last.
    struct View {
        bool running = false, finished = false, cancelled = false, failed_to_run = false;
        std::uint64_t done = 0, total = 0, failed = 0, hlsl = 0, lift = 0;
        std::uint64_t recipes_done = 0, recipes_total = 0;  // pipelines from the manifest and the shipped list
        double seconds = 0, eta = -1;
        std::string phase, error;
        std::vector<std::string> reasons;
    };
    View view() const;

private:
    void reader();
    mutable std::mutex mu_;
    View v_;
    void* proc_ = nullptr;  // SDL_Process
    std::thread thread_;
};
