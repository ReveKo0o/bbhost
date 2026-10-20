#include "net/discord_presence.h"
#include "discord_rpc.h" // official title within third_party
#include <chrono>
#include <string>
#include <cstring>

namespace net {

    void InitDiscordRPC() {
        DiscordEventHandlers handlers;
        std::memset(&handlers, 0, sizeof(handlers));

        // Developer Portal Application id (BBHost)
        Discord_Initialize("1562063078466650206", &handlers, 1, nullptr);
    }

    void UpdateDiscordPresence(const char* mapName, int playerLevel) {
        DiscordRichPresence discordPresence;
        std::memset(&discordPresence, 0, sizeof(discordPresence));

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

    void ShutdownDiscordRPC() {
        Discord_Shutdown();
    }

} // namespace net