#include "discord_rpc.h"
#include <chrono>
#include <string>
#include <cstring>

// Function to Initialize Discord RPC
void InitDiscordRPC() {
    DiscordEventHandlers handlers;
    memset(&handlers, 0, sizeof(handlers));

    // Application ID: 1562063078466650206
    Discord_Initialize("1562063078466650206", &handlers, 1, nullptr);
}

// Presence Update Function
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
    discordPresence.largeImageText = "Bloodborne PC (bbhost)";

    Discord_UpdatePresence(&discordPresence);
}
