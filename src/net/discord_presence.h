#pragma once

namespace net {

	// Launch the discord presence service
	void InitDiscordRPC();

	// The player posts their current map and level on Discord
	void UpdateDiscordPresence(const char* mapName, int playerLevel);

	// Terminates the RPC connection upon closing
	void ShutdownDiscordRPC();

} // namespace net