#include "engine/change_appearance.h"

#include "core/config.h"
#include "core/elf.h"
#include "core/thunk.h"
#include "engine/addr.h"
#include "engine/dream_mirror_layout.h"
#include "engine/menu_steps.h"
#include "engine/world_chr.h"
#include "gcn/container.h"
#include "hle/fs.h"
#include "log.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Bumped whenever the rewrite changes, so installs remake the file.
constexpr const char* kStamp = "dream-mirror 1";
constexpr const char* kLayout = "dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx";

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& p, const std::vector<std::uint8_t>& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}


// --- the editor's frame ----------------------------------------------------
//
// The list the talk command opens is character creation's Appearance list
// alone. In character creation it sits in the ChrMake_BG window
// (chrmake_bg.gfx: the book, the panels, the model): its controller
// (sub_1f06b10) makes the preview scenes and their render targets, binds the
// menu's placeholder images to them and, in its update (sub_1f07870), copies
// the hunter's face data into them, swaps them in as each loads, turns the
// mouse and the zoom keys into the model's rotation and distance, and picks
// the image the panel shows. Character creation's top controller (sub_1f8dbb0)
// opens that window from a window descriptor (sub_1ed8150), and its menus
// update it every frame as a dialog's child window (sub_1ed5b10, the base
// update, walks them); at the mirror nothing does either, so the list floats
// over the world with an empty panel and the HUD on top.
//
// Here: the list's window step being made (engine/menu_steps.h: type 7,
// "ChrMakeCommandList") while the hunter is in the world - character creation
// runs at the title, with its own window - opens ChrMake_BG with a descriptor
// of ours, filled as character creation fills its own; each frame its
// controller's update runs; and a reference we hold on the list's step tells
// when the list has closed, which closes the window (sub_1ed8e60).
constexpr std::uint64_t kListType = 7;
constexpr char16_t kListName[] = u"ChrMakeCommandList";
constexpr std::uint64_t kOpenWindow = 0x1ed8150;   // (descriptor)
constexpr std::uint64_t kCloseWindow = 0x1ed8e60;  // (descriptor): closes the window, unloads the movie
constexpr std::uint64_t kMenuHeap = 0x5940418;     // the menus' allocator: +0x58 alloc(size, align), +0x70 free(p)
constexpr std::size_t kDescSize = 0xc0;
constexpr std::uint64_t kDescVtable = 0x5736b30, kDescOwnerVtable = 0x5736b70, kDescListVtable = 0x57310d0;
constexpr std::uint64_t kFactoryVtable = 0x573ee10;  // the functor character creation stores ...
constexpr std::uint64_t kFrameFactory = 0x1f08010;   // ... around this factory: sub_1f06b10's controller
constexpr std::uint64_t kFrameName = 0x4d98486;      // L"ChrMake_BG"
constexpr std::uint64_t kFrameType = 6;

std::uint64_t g_slide = 0;
int g_watch = -1;                          // the list's steps (engine/menu_steps.h)
std::uint64_t g_list = 0;                  // the list's step, while we hold a reference
std::uint64_t g_desc = 0;                  // our descriptor, while ChrMake_BG is open
std::uint8_t g_active = 1;                 // the update's argument (a flag byte it reads)
int g_opened = 0;                          // for the log

std::uint64_t slot(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }
std::uint64_t rd64(std::uint64_t a) {
    std::uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof v);
    return v;
}
void wr64(std::uint64_t a, std::uint64_t v) { std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)), &v, sizeof v); }
void* code(std::uint64_t a) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)); }

std::uint64_t heap_alloc(std::size_t n) {
    const std::uint64_t heap = rd64(slot(kMenuHeap));
    if (!heap) return 0;
    return static_cast<std::uint64_t>(hle_call_guest<std::int64_t>(code(rd64(rd64(heap) + 0x58)), heap, n, 0x10));
}
void heap_free(std::uint64_t p) {
    const std::uint64_t heap = rd64(slot(kMenuHeap));
    if (heap && p) hle_call_guest<std::int64_t>(code(rd64(rd64(heap) + 0x70)), heap, p);
}

bool open_frame() {
    const std::uint64_t d = heap_alloc(kDescSize);
    if (!d) return false;
    std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d)), 0, kDescSize);
    wr64(d + 0x00, slot(kDescVtable));
    wr64(d + 0x08, slot(kDescOwnerVtable));
    wr64(d + 0x18, kFrameType);
    wr64(d + 0x20, slot(kFrameName));
    wr64(d + 0x30, slot(kFactoryVtable));  // the factory functor, in the descriptor's own storage
    wr64(d + 0x38, slot(kFrameFactory));
    wr64(d + 0x50, d + 0x30);
    wr64(d + 0x68, slot(kDescListVtable));
    hle_call_guest<std::int64_t>(code(slot(kOpenWindow)), d);
    if (!rd64(d + 0x60)) {  // no window: nothing to keep
        hle_call_guest<std::int64_t>(code(slot(kCloseWindow)), d);
        heap_free(d);
        return false;
    }
    g_desc = d;
    return true;
}

void close_frame() {
    hle_call_guest<std::int64_t>(code(slot(kCloseWindow)), g_desc);
    heap_free(g_desc);
    g_desc = 0;
}

void release_list() {
    menu_step_release(g_list);
    g_list = 0;
}

bool g_frame_on = false;

}  // namespace

void change_appearance_tick() {
    if (!g_frame_on) return;
    if (const std::uint64_t made = menu_steps_take(g_watch)) {
        std::uint32_t block = 0;
        if (!g_list && !g_desc && world_player_block(&block) && *menu_step_count(made) >= 1 && open_frame()) {
            menu_step_hold(made);  // ours: when it is the last, the list has closed
            g_list = made;
            if (g_opened++ < 4) host_log("change appearance: the editor's frame opened (ChrMake_BG, preview of the hunter)");
        }
    }
    if (g_desc) {
        const std::uint64_t ctl = rd64(g_desc + 0x60);
        if (ctl) hle_call_guest<std::int64_t>(code(rd64(rd64(ctl) + 0x18)), ctl, &g_active);
    }
    if (g_list && *menu_step_count(g_list) == 1) {
        close_frame();
        release_list();
    }
}

void change_appearance_install(ElfImage* image) {
    if (!config().change_appearance) {
        host_log("change appearance: off (PC enhancements: Hunter's Dream mirror)");
        return;
    }
    if (!image || image->sha256 != kEboot109Sha256) {
        host_log("change appearance: off - not the 1.09 eboot");
        return;
    }
    const char* app0 = hle_fs_app0_root();
    const char* data = hle_fs_data_root();
    if (!app0 || !*app0 || !data || !*data) {
        host_log("change appearance: off - no dump or data folder");
        return;
    }
    const fs::path out = fs::path(data) / "bbhost" / "world-assets";
    const auto have = read_file(out / "stamp");
    const bool fresh = have && std::string(have->begin(), have->end()) == kStamp && fs::exists(out / kLayout);
    if (!fresh) {
        const auto src = read_file(hle_fs_game_file(kLayout));
        if (!src) {
            host_log("change appearance: off - the dump has no %s", kLayout);
            return;
        }
        std::string why;
        std::vector<std::uint8_t> msb = gcn::dcx_decompress(*src, &why);
        if (msb.empty()) {
            host_log("change appearance: off - %s does not inflate (%s)", kLayout, why.c_str());
            return;
        }
        if (!dream_mirror_layout(msb, &why)) {
            host_log("change appearance: off - %s", why.c_str());
            return;
        }
        const std::vector<std::uint8_t> packed = gcn::dcx_compress(msb);
        const std::string stamp = kStamp;
        if (packed.empty() || !write_file(out / kLayout, packed) ||
            !write_file(out / "stamp", std::vector<std::uint8_t>(stamp.begin(), stamp.end()))) {
            host_log("change appearance: off - cannot write %s", out.string().c_str());
            return;
        }
        host_log("change appearance: made the Dream's layout with the mirror's character (%zu bytes) in %s", packed.size(),
                 out.string().c_str());
    }
    hle_fs_add_generated_root(out.string().c_str());
    g_slide = image->mem.slide;
    g_watch = menu_steps_watch(kListType, kListName);
    g_frame_on = g_watch >= 0 && menu_steps_install(image);
    host_log("change appearance: the mirror in the Hunter's Dream opens the appearance editor%s",
             g_frame_on ? ", in character creation's frame" : " (its frame NOT hooked: unexpected bytes)");
}
