#pragma once

// The follow camera, from LockCamParam - one row per area's camera setup,
// which the camera's update (sub_183ac60) reads every frame and eases toward:
// its distance (camDistTarget), the height it looks at (chrOrgOffset_Y) and
// its vertical field of view (camFovY, 43 degrees in nearly every row). The
// settings scale those fields in the table the game has loaded.
// engine/camera.cpp.

struct ElfImage;
// Lifts the camera's 48-degree ceiling on camFovY. Only after the eboot's
// hash has been checked, like every address-based patch.
void camera_install(ElfImage* image);

// Called for every view (engine/graphics_patch.cpp's render-view hook): finds
// the table once, then keeps it at the settings.
void camera_tick();
