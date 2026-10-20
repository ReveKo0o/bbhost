#include "net/discord_presence.h"

#include "discord_rpc.h"

#include <cstring>
#include <string>

namespace net {
namespace {

constexpr char kApplicationId[] = "1562063078466650206";
bool g_initialized = false;

void publish(const char* state) {
    DiscordRichPresence presence{};
    presence.details = "Bloodborne";
    presence.state = state;
    Discord_UpdatePresence(&presence);
}

}  // namespace

void InitDiscordRPC() {
    if (g_initialized) return;

    DiscordEventHandlers handlers{};
    Discord_Initialize(kApplicationId, &handlers, 1, nullptr);
    g_initialized = true;
}

void UpdateDiscordLauncherPresence() {
    if (!g_initialized) return;
    publish("In BBHost Launcher");
}

void UpdateDiscordGamePresence(const char* mapName, int playerLevel) {
    if (!g_initialized) return;

    // Do not report a character as loaded until the guest exposes a valid level.
    std::string state = mapName && mapName[0] ? mapName : playerLevel > 0 ? "Character loaded" : "Loading character...";
    if (playerLevel > 0) state += " · Level " + std::to_string(playerLevel);
    publish(state.c_str());
}

void ShutdownDiscordRPC() {
    if (!g_initialized) return;

    Discord_ClearPresence();
    Discord_Shutdown();
    g_initialized = false;
}

}  // namespace net
