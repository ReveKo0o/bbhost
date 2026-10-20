#include "discord_rpc.h"
#include <chrono>
#include <string>
#include <cstring>

// Discord RPC SDK Stub / Mock Implementation
extern "C" {

void Discord_Initialize(const char* applicationId, DiscordEventHandlers* handlers, int autoRegister, const char* optionalSteamId) {
    //Discord client launch body
}

void Discord_Shutdown(void) {
}

void Discord_RunCallbacks(void) {
}

void Discord_UpdatePresence(const DiscordRichPresence* presence) {
}

void Discord_ClearPresence(void) {
}

void Discord_Respond(const char* userId, int reply) {
}

void Discord_UpdateHandlers(DiscordEventHandlers* handlers) {
}

} // extern "C"

void InitDiscordRPC() {
    DiscordEventHandlers handlers;
    memset(&handlers, 0, sizeof(handlers));
    Discord_Initialize("1562063078466650206", &handlers, 1, nullptr);
}

void UpdatePlayerPresence(const char* mapName, int playerLevel) {
    DiscordRichPresence discordPresence;
    memset(&discordPresence, 0, sizeof(discordPresence));

    std::string detailsStr = "Location: " + std::string(mapName);
    discordPresence.details = detailsStr.c_str();

    std::string stateStr = "Hunter Level: " + std::to_string(playerLevel);
    discordPresence.state = stateStr.c_str();

    discordPresence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    discordPresence.largeImageKey = "bb_logo";
    discordPresence.largeImageText = "bbhost";

    Discord_UpdatePresence(&discordPresence);
}
