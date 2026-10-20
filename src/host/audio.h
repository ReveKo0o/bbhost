#pragma once

#include <cstddef>
#include <cstdint>

// Host audio output over SDL. Returns 0 when no device is available; the
// caller then paces the mixer by time instead.
int host_audio_open(unsigned freq, unsigned channels, bool is_float, unsigned frames_per_write);
// Queue one buffer (frames * channels samples). Blocks while more than a few
// buffers are queued so the guest mixer runs at real-time rate.
void host_audio_write(int handle, const void* data, unsigned frames);
void host_audio_set_gain(int handle, float gain);
// Waits for writes in progress on the handle, then destroys its stream.
void host_audio_close(int handle);
// Before SDL_Quit: stops every output (writes in progress finish first, later
// writes return at once) and shuts the audio subsystem down.
void host_audio_shutdown();
// Every output's device paused at once, nothing waited for: the game's sound
// stops while bbhost says why it has to end.
void host_audio_pause_all();
