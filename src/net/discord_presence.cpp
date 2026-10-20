#include "net/discord_presence.h"

#include "discord_rpc.h"

#include <cstring>
#include <string>

namespace net {
namespace {

constexpr char kApplicationId[] = "1562063078466650206";
bool g_initialized = false;
std::string g_last_activity;

void publish(const char* state) {
    if (!state || g_last_activity == state) return;
    DiscordRichPresence presence{};
    presence.details = "Bloodborne";
    presence.state = state;
    Discord_UpdatePresence(&presence);
    g_last_activity = state;
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

void UpdateDiscordMainMenuPresence() {
    if (!g_initialized) return;
    publish("Main Menu");
}

void UpdateDiscordGamePresence(const char* characterName, int playerLevel) {
    if (!g_initialized) return;

    // Do not report a character as loaded until the guest exposes a valid level.
    std::string state = characterName && characterName[0] ? characterName : playerLevel > 0 ? "Character loaded" : "Loading character...";
    if (playerLevel > 0) state += " · Level " + std::to_string(playerLevel);
    publish(state.c_str());
}

void ShutdownDiscordRPC() {
    if (!g_initialized) return;

    Discord_ClearPresence();
    Discord_Shutdown();
    g_initialized = false;
    g_last_activity.clear();
}

}  // namespace net
