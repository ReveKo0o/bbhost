#pragma once

namespace net {

// Starts the Discord IPC client once for the current BBHost process.
void InitDiscordRPC();

// Publishes a lightweight status while the setup/launcher window is open.
void UpdateDiscordLauncherPresence();

// Publishes the game's location and loaded character level when available.
void UpdateDiscordGamePresence(const char* mapName, int playerLevel);

// Clears the activity and closes the IPC client.
void ShutdownDiscordRPC();

}  // namespace net
