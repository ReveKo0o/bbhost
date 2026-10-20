// Common-dialog family (message, IME, NP commerce, NP profile). There is no
// UI yet: every dialog goes INITIALIZED -> RUNNING -> FINISHED on the next
// status poll and reports the neutral result (OK / cancelled) so the game's
// state machine advances.
#include "hle/common.h"
#include "hle/hle.h"
#include "hle/modules.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "host/window.h"

namespace {

// SceCommonDialogStatus
constexpr int kNone = 0;
constexpr int kInitialized = 1;
constexpr int kRunning = 2;
constexpr int kFinished = 3;
constexpr int kErrNotInitialized = static_cast<int>(0x80B80001u);
constexpr int kErrAlreadyInit = static_cast<int>(0x80B80002u);

struct DialogState {
    std::mutex mu;
    int status = kNone;
    int polls = 0;
    const char* name;
    explicit DialogState(const char* n) : name(n) {}

    int init() {
        std::lock_guard<std::mutex> lock(mu);
        if (status != kNone && status != kFinished) {
            return kErrAlreadyInit;
        }
        status = kInitialized;
        return 0;
    }
    int open() {
        std::lock_guard<std::mutex> lock(mu);
        if (status == kNone) {
            return kErrNotInitialized;
        }
        status = kRunning;
        polls = 0;
        host_log("HLE fake: %sOpen (auto-finished)", name);
        return 0;
    }
    int update() {
        std::lock_guard<std::mutex> lock(mu);
        if (status == kRunning && ++polls >= 2) {
            status = kFinished;
        }
        return status;
    }
    int term() {
        std::lock_guard<std::mutex> lock(mu);
        status = kNone;
        return 0;
    }
};

DialogState g_msg("sceMsgDialog");
std::mutex g_ime_mu;  // sceImeDialog keeps its own state (ImeSession), not a DialogState
DialogState g_commerce("sceNpCommerceDialog");
DialogState g_profile("sceNpProfileDialog");

// ---- sceMsgDialog
GUEST_ABI int hle_msg_init() { return g_msg.init(); }
GUEST_ABI int hle_msg_open(const void*) { return g_msg.open(); }
GUEST_ABI int hle_msg_update() { return g_msg.update(); }
GUEST_ABI int hle_msg_term() { return g_msg.term(); }

// ---- sceImeDialog: text entry. Windowed, the host window collects the
// typed text (a box over the game shows it; Enter accepts, Escape cancels)
// and it is written back to the game's buffer as UTF-16. Headless, the
// configured name is entered.
//
// SceImeDialogParam: +4 type, +16 enterLabel, +32 option, +36 maxTextLength,
// +40 wchar16* inputTextBuffer, +72 const wchar16* title.
// SceImeDialogResult: +0 endstatus (0 OK, 1 USER_CANCELED, 2 ABORTED).
// sceImeDialog does NOT use SceCommonDialogStatus: it has its own three-value
// SceImeDialogStatus (NONE 0, RUNNING 1, FINISHED 2). Bloodborne's name entry
// polls GetStatus and calls GetResult the moment it sees 2, so returning the
// common-dialog RUNNING (2) made the game read the result while the dialog was
// still up - endstatus was still USER_CANCELED, so every name was thrown away
// and the name screen never accepted anything.
constexpr int kImeNone = 0, kImeRunning = 1, kImeFinished = 2;
// sceIme error codes are their own family (0x80bc....), not the common
// dialog's 0x80b8.....
constexpr int kErrImeBusy = static_cast<int>(0x80bc0001u);
constexpr int kErrImeInvalidAddress = static_cast<int>(0x80bc0031u);
constexpr int kErrImeNotFinished = static_cast<int>(0x80bc0106u);
constexpr std::uint32_t kImeTypeBasicLatin = 1;     // SCE_IME_TYPE_BASIC_LATIN
constexpr std::uint32_t kImeEnterLabelSearch = 2;   // SCE_IME_ENTER_LABEL_SEARCH
constexpr std::uint32_t kImeOptionPassword = 0x4;   // SCE_IME_OPTION_PASSWORD

struct ImeSession {
    std::uint16_t* buffer = nullptr;
    unsigned max_chars = 0;
    TextCharset charset = TextCharset::Any;
    bool windowed = false;
    int status = kImeNone;
    int polls = 0;
    int end_status = 1;
    bool done = false;
};
ImeSession g_ime_session;
std::string g_default_name = "Hunter";
// The three text fields the game opens, each built by its own function in
// front of the one sceImeDialogInit call (sub_2d03f80): the character's name
// (sub_24169f0, "Please Enter Name") is 16 characters; the chalice glyph
// (sub_2416b50, "Enter Chalice Glyph") and the network password (sub_2416cb0,
// "Enter password") are 8. All three are basic Latin with the option
// NO_LEARNING (0x20) alone - so no PASSWORD, and the console shows the network
// password as it is typed - and only the glyph's Enter key reads Search. Only
// the name starts from player.name; the others start blank and wait.
constexpr unsigned kNameChars = 16;
bool g_ime_type = false;  // player.ime = "type"

// The characters a field takes, from its dialog's parameters (host/text_entry.h).
// A glyph is the basic-Latin field whose Enter key reads Search; it takes only
// the glyph alphabet, which is all the game's own glyph editor offers.
TextCharset ime_charset(std::uint32_t type, std::uint32_t enter_label) {
    if (type != kImeTypeBasicLatin) return TextCharset::Any;
    return enter_label == kImeEnterLabelSearch ? TextCharset::Glyph : TextCharset::BasicLatin;
}

std::string utf16_to_utf8(const std::uint16_t* p, unsigned max) {
    std::string out;
    for (unsigned i = 0; p && i < max && p[i]; ++i) {
        const std::uint32_t c = p[i];
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xc0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
        else { out.push_back(static_cast<char>(0xe0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
    }
    return out;
}
void utf8_to_utf16(const std::string& in, std::uint16_t* out, unsigned max) {
    unsigned n = 0;
    for (std::size_t i = 0; i < in.size() && n < max;) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::uint32_t cp;
        std::size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xe0) == 0xc0 && i + 1 < in.size()) { cp = ((c & 0x1f) << 6) | (in[i + 1] & 0x3f); len = 2; }
        else if ((c & 0xf0) == 0xe0 && i + 2 < in.size()) { cp = ((c & 0x0f) << 12) | ((in[i + 1] & 0x3f) << 6) | (in[i + 2] & 0x3f); len = 3; }
        else { cp = '?'; len = 1; }
        if (cp > 0xffff) cp = '?';
        out[n++] = static_cast<std::uint16_t>(cp);
        i += len;
    }
    out[n] = 0;
}

GUEST_ABI int hle_ime_init(const std::uint8_t* param, const void*) {
    std::lock_guard<std::mutex> lock(g_ime_mu);
    ImeSession& se = g_ime_session;
    if (se.status != kImeNone) return kErrImeBusy;
    se = ImeSession{};
    se.status = kImeRunning;
    std::string title;
    std::uint32_t type = 0, enter_label = 0, option = 0;
    if (param) {
        std::memcpy(&type, param + 4, 4);
        std::memcpy(&enter_label, param + 16, 4);
        std::memcpy(&option, param + 32, 4);
        std::memcpy(&se.max_chars, param + 36, 4);
        std::uint64_t buf = 0;
        std::memcpy(&buf, param + 40, 8);
        se.buffer = reinterpret_cast<std::uint16_t*>(static_cast<std::uintptr_t>(buf));
        std::uint64_t title_va = 0;
        std::memcpy(&title_va, param + 72, 8);
        title = utf16_to_utf8(reinterpret_cast<const std::uint16_t*>(static_cast<std::uintptr_t>(title_va)), 64);
        host_log("sceImeDialogInit: \"%s\" max %u chars, type %u, enter label %u, option 0x%x, current \"%s\"", title.c_str(),
                 se.max_chars, type, enter_label, option, utf16_to_utf8(se.buffer, se.max_chars).c_str());
    }
    if (se.max_chars == 0 || se.max_chars > 256) se.max_chars = 32;
    se.charset = ime_charset(type, enter_label);
    // player.ime = "auto" answers the name with player.name at once. A glyph
    // or a password has no answer to give, so with a window it is typed like
    // "type" would have it - cancelling it the moment it opened, as this did,
    // left the chalice glyph search and the network password impossible to
    // use - and only a run with no window to type in (headless) cancels it.
    const bool name = se.max_chars == kNameChars;
    se.windowed = host_window_active() && (g_ime_type || !name);
    if (se.windowed) {
        // Start from whatever the game already has. The name, when it has
        // nothing, starts from player.name, so Enter on its own is a valid
        // answer - selected, so the first key typed replaces it rather than
        // adding to it ("HunterGehrman", with six of the sixteen characters
        // gone); a glyph or a password starts blank.
        std::string initial = utf16_to_utf8(se.buffer, se.max_chars);
        const bool suggestion = initial.empty() && name;
        if (suggestion) initial = g_default_name;
        host_text_entry_begin(initial.c_str(), se.max_chars, (option & kImeOptionPassword) != 0, title.c_str(), se.charset,
                              suggestion);
        host_log("sceImeDialog: type the text in the game window (the box over the game shows it); Enter accepts, Escape cancels, Ctrl+V pastes");
    } else if (name) {
        host_log("sceImeDialog: %s; answering with \"%s\" (player.name)",
                 host_window_active() ? "player.ime = \"auto\"" : "no window to type in", g_default_name.c_str());
    } else {
        host_log("sceImeDialog: no window to type in; cancelling \"%s\" (only the name has an answer)", title.c_str());
    }
    return 0;
}
// The first calls of every sceImeDialog entry point: the order the game uses
// decides where the entered text has to be written back.
int g_ime_trace = 0;
void ime_trace(const char* what, int extra) {
    if (g_ime_trace++ < 40) host_log("sceImeDialog%s (%d)", what, extra);
}

GUEST_ABI int hle_ime_status() {
    ImeSession& se = g_ime_session;
    std::lock_guard<std::mutex> lock(g_ime_mu);
    ime_trace("GetStatus", se.status);
    if (se.status != kImeRunning) return se.status;
    if (se.done) return se.status = kImeFinished;
    std::string text;
    if (se.windowed) {
        const int r = host_text_entry_poll(text);
        if (r == 0) return kImeRunning;
        host_text_entry_end();
        se.end_status = r == 1 ? 0 : 1;
    } else {
        if (++se.polls < 2) return kImeRunning;
        // The name as the field would have taken it typed: its characters
        // and its length.
        TextEntry answer;
        answer.begin(g_default_name.c_str(), se.max_chars, se.charset, false);
        text = answer.text;
        se.end_status = se.max_chars == kNameChars ? 0 : 1;  // a glyph or password has no answer: cancelled
    }
    if (se.end_status == 0 && se.buffer) {
        // NOTE: do not gate this on hle_kernel_va_mapped(). That helper only
        // knows about direct-memory maps, so it reports false for the game's
        // own heap and for the eboot's data segment - which is where this
        // buffer actually lives. Gating on it silently threw the entered name
        // away and left the name-entry screen stuck. sceImeDialogInit already
        // reads this same pointer unguarded, so writing it is no less safe.
        utf8_to_utf16(text, se.buffer, se.max_chars);
        host_log("sceImeDialog: wrote \"%s\" (%u chars max) to %p", text.c_str(), se.max_chars,
                 static_cast<void*>(se.buffer));
    } else if (se.end_status == 0) {
        host_log("sceImeDialog: accepted \"%s\" but the game gave no text buffer", text.c_str());
    }
    se.done = true;
    return se.status = kImeFinished;
}
GUEST_ABI int hle_ime_result(std::int32_t* result) {
    std::lock_guard<std::mutex> lock(g_ime_mu);
    ime_trace("GetResult", g_ime_session.end_status);
    if (!result) return kErrImeInvalidAddress;
    if (g_ime_session.status != kImeFinished) return kErrImeNotFinished;
    // SceImeDialogResult is endstatus plus reserved words; the caller's buffer
    // is only 16 bytes on the game's stack, so do not clear more than that.
    std::memset(result, 0, 16);
    result[0] = g_ime_session.end_status;
    return 0;
}
GUEST_ABI int hle_ime_abort() {
    std::lock_guard<std::mutex> lock(g_ime_mu);
    ime_trace("Abort", g_ime_session.status);
    if (g_ime_session.windowed) host_text_entry_end();
    g_ime_session.end_status = 2;  // ABORTED
    g_ime_session.status = kImeNone;
    return 0;
}
GUEST_ABI int hle_ime_term() {
    std::lock_guard<std::mutex> lock(g_ime_mu);
    ime_trace("Term", g_ime_session.status);
    if (g_ime_session.windowed) host_text_entry_end();
    g_ime_session.status = kImeNone;
    return 0;
}

// ---- sceNpCommerceDialog (store)
GUEST_ABI int hle_commerce_init() { return g_commerce.init(); }
GUEST_ABI int hle_commerce_open(const void*) { return g_commerce.open(); }
GUEST_ABI int hle_commerce_update() { return g_commerce.update(); }
GUEST_ABI int hle_commerce_term() { return g_commerce.term(); }

// ---- sceNpProfileDialog
GUEST_ABI int hle_profile_init() { return g_profile.init(); }
GUEST_ABI int hle_profile_open(const void*) { return g_profile.open(); }
GUEST_ABI int hle_profile_update() { return g_profile.update(); }
GUEST_ABI int hle_profile_result(std::int32_t* result) {
    if (result) {
        std::memset(result, 0, 32);
    }
    return 0;
}
GUEST_ABI int hle_profile_term() { return g_profile.term(); }

}  // namespace

void hle_dialog_set_default_name(const char* name, bool type_in_window) {
    if (name && name[0]) g_default_name = name;
    g_ime_type = type_in_window;
}

void hle_register_dialog() {
#define REG(name, fn) register_hle_fn(name, reinterpret_cast<void*>(fn))
    REG("sceMsgDialogInitialize", hle_msg_init);
    REG("sceMsgDialogOpen", hle_msg_open);
    REG("sceMsgDialogUpdateStatus", hle_msg_update);
    REG("sceMsgDialogTerminate", hle_msg_term);
    REG("sceImeDialogInit", hle_ime_init);
    REG("sceImeDialogGetStatus", hle_ime_status);
    REG("sceImeDialogGetResult", hle_ime_result);
    REG("sceImeDialogAbort", hle_ime_abort);
    REG("sceImeDialogTerm", hle_ime_term);
    REG("sceNpCommerceDialogInitialize", hle_commerce_init);
    REG("sceNpCommerceDialogOpen", hle_commerce_open);
    REG("sceNpCommerceDialogUpdateStatus", hle_commerce_update);
    REG("sceNpCommerceDialogTerminate", hle_commerce_term);
    REG("sceNpProfileDialogInitialize", hle_profile_init);
    REG("sceNpProfileDialogOpen", hle_profile_open);
    REG("sceNpProfileDialogUpdateStatus", hle_profile_update);
    REG("sceNpProfileDialogGetResult", hle_profile_result);
    REG("sceNpProfileDialogTerminate", hle_profile_term);
#undef REG
}
