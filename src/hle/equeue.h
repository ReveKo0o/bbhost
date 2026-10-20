#pragma once

#include "core/futex.h"

#include <cstdint>
#include <deque>
#include <string>

struct HostEvent {
    std::uint64_t ident = 0;
    std::int16_t filter = 0;
    std::uint16_t flags = 0;
    std::uint32_t fflags = 0;
    std::intptr_t data = 0;
    void* udata = nullptr;
};

// The game's flip and GPU events arrive here from the vblank clock and the
// command processor. Its lock and sleep are core/futex.h's (WaitOnAddress on
// Windows, not winpthreads'), so a short timed wait is slept out on the
// high-resolution timer rather than rounded to the system tick.
struct HostEqueue {
    HostLock mu;
    HostCondVar cv;
    std::string name;
    std::deque<HostEvent> q;
    bool dead = false;
    int waiters = 0;
};

bool equeue_live(HostEqueue* eq);
void equeue_post(HostEqueue* eq, const HostEvent& ev);
