#pragma once

// BBHOST_FMOD_PROBE=1: why FMOD's AT9 codec stops a sound. A hook on the
// codec's job-completion step (0x1119c00, which FMOD calls when a channel's
// decoded-PCM ring runs low) logs, for every call that will fail, which of
// its checks fails. Diagnostic only; nothing is changed.

struct ElfImage;

void fmod_probe_install(ElfImage* image);
