#pragma once

#include <cstdint>

struct ElfImage;

// Bind the loaded 1.09 image so named layouts can be read. No-op if the
// eboot hash does not match. Call again with nullptr to unbind.
void engine_bind(ElfImage* image);

// Cheap. Logs SprjFlipper once the singleton exists. Safe off the video lock.
void engine_on_flip(std::uint64_t flip_count);
