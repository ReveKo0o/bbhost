// The in-game plugin menu (F9): the plugin manager (host/plugin_ui.h) over
// the game - every plugin's settings, changed at once where a plugin says
// they can be, and its actions (Start the boss rush). Dear ImGui drawn into
// the presented frame with its Vulkan backend, after the host overlay. While
// it is open it takes the keyboard, the mouse and the pad, as F10's screen
// does.
//
// Two threads: the window's event pump hands its SDL events here
// (ingame_menu_event), and the presenter builds and records the menu
// (ingame_menu_record) - the ImGui context lives on the presenter, fed from a
// queue.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

union SDL_Event;

bool ingame_menu_open();
// Pump thread. F9 opens and closes it (Escape closes); while open every
// input event is taken. `pixel_ratio`: the window's pixels per point.
// Returns true when the event was the menu's.
bool ingame_menu_event(const SDL_Event& e, float pixel_ratio);
// Presenter. Once, with the swapchain's device; again after a swapchain
// rebuild changes the image count or the format.
bool ingame_menu_init(VkInstance instance, VkPhysicalDevice phys, VkDevice device, std::uint32_t family, VkQueue queue,
                      VkFormat format, std::uint32_t image_count);
void ingame_menu_shutdown();
// Records the menu over `view` (an image in the colour-attachment layout).
// Nothing when it is closed.
void ingame_menu_record(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent);
// The same draws into a rendering the caller has begun over an image of
// `extent` (the presenter's one pass); false, and nothing, when it is closed.
bool ingame_menu_record_in_pass(VkCommandBuffer cmd, VkExtent2D extent);
