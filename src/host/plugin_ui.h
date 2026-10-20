// The plugin manager: one Dear ImGui panel, drawn in the setup window's
// Plugins tab before the game starts and in the in-game plugin menu. It
// lists every plugin file (official ones marked), turns plugins on and off,
// fetches the latest official plugins, and draws each plugin's settings
// from the options table it exports (bbhost_plugin.h, BbOption) - with its
// action buttons (Start the boss rush) when the game is running.
#pragma once

#include "host/plugins.h"

#include <map>
#include <string>
#include <vector>

struct PluginUi {
    bool in_game = false;           // actions work; settings marked live change at once
    std::vector<PluginEntry> plugins;
    int selected = 0;
    std::map<std::string, std::string> edits;  // text fields being typed, by plugin.key
    bool scanned = false;
};

// Reads the plugin files again (after a fetch, or when the panel opens).
void plugin_ui_refresh(PluginUi& ui);
// Draws the panel into the current ImGui window, filling `height` (0: the
// rest of the window).
void plugin_ui_draw(PluginUi& ui, float scale, float height = 0.0f);
// Plugins the player has enabled that carry settings and have not been seen
// in the plugin manager yet - the setup window opens on its Plugins tab for
// them (and plugins.show_settings = true opens it at every start).
bool plugin_ui_wants_attention();
// Marks the enabled plugins as seen (the setup window, once shown).
void plugin_ui_mark_seen(const PluginUi& ui);
