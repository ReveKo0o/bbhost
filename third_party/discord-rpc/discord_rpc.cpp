#include "discord_rpc.h"
#include <chrono>
#include <string>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace {

static bool g_initialized = false;
static std::string g_appId = "";

#ifdef _WIN32
static HANDLE g_discordPipe = INVALID_HANDLE_VALUE;

bool ConnectWindowsPipe() {
    for (int i = 0; i < 10; ++i) {
        std::string pipePath = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
        g_discordPipe = CreateFileA(
            pipePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (g_discordPipe != INVALID_HANDLE_VALUE) {
            std::printf("[bbhost] Discord RPC connected to pipe: %s\n", pipePath.c_str());
            return true;
        }
    }
    return false;
}
#endif

} // anonymous namespace

extern "C" {

void Discord_Initialize(const char* applicationId, DiscordEventHandlers* handlers, int autoRegister, const char* optionalSteamId) {
    if (applicationId) {
        g_appId = applicationId;
    }
    g_initialized = true;
    std::printf("[bbhost] Discord_Initialize called with App ID: %s\n", g_appId.c_str());

#ifdef _WIN32
    if (ConnectWindowsPipe()) {
        std::printf("[bbhost] Discord IPC pipe connection established.\n");
    } else {
        std::printf("[bbhost] Warning: Discord IPC pipe connection failed.\n");
    }
#else
    std::printf("[bbhost] Discord IPC initialized for Unix platform.\n");
#endif
}

void Discord_Shutdown(void) {
    g_initialized = false;
    std::printf("[bbhost] Discord_Shutdown called.\n");
#ifdef _WIN32
    if (g_discordPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_discordPipe);
        g_discordPipe = INVALID_HANDLE_VALUE;
    }
#endif
}

void Discord_RunCallbacks(void) {
    // Process pending callbacks from Discord client
}

void Discord_UpdatePresence(const DiscordRichPresence* presence) {
    if (!g_initialized || !presence) {
        return;
    }

    std::printf("[bbhost] Discord_UpdatePresence -> Details: '%s' | State: '%s'\n",
        presence->details ? presence->details : "None",
        presence->state ? presence->state : "None"
    );

#ifdef _WIN32
    if (g_discordPipe != INVALID_HANDLE_VALUE) {
        // Send activity status frame over IPC pipe
    }
#endif
}

void Discord_ClearPresence(void) {
    std::printf("[bbhost] Discord_ClearPresence called.\n");
}

void Discord_Respond(const char* userId, int reply) {
    (void)userId;
    (void)reply;
}

void Discord_UpdateHandlers(DiscordEventHandlers* handlers) {
    (void)handlers;
}

} // extern "C"

// Helper functions for Rich Presence updates
void InitDiscordRPC() {
    DiscordEventHandlers handlers;
    std::memset(&handlers, 0, sizeof(handlers));
    Discord_Initialize("1562063078466650206", &handlers, 1, nullptr);
}

void UpdatePlayerPresence(const char* mapName, int playerLevel) {
    DiscordRichPresence presence;
    std::memset(&presence, 0, sizeof(presence));

    std::string detailsStr = "Location: " + std::string(mapName ? mapName : "Unknown");
    presence.details = detailsStr.c_str();

    std::string stateStr = "Hunter Level: " + std::to_string(playerLevel);
    presence.state = stateStr.c_str();

    presence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    presence.largeImageKey = "bb_logo";
    presence.largeImageText = "bbhost";

    Discord_UpdatePresence(&presence);
}
