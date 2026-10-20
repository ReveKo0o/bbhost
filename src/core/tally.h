#pragma once

// A statistic many threads bump on a hot path. One shared atomic bounced its
// cache line between the GX workers on every draw (their contended fetch_adds
// were ~2% of the workers' time); a Tally gives each thread a slot on its own
// line, and a reader sums the slots. Adds wrap, so a gauge may add -1.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tally_detail {
constexpr std::size_t kSlots = 16;
inline std::size_t slot() {
    static std::atomic<std::size_t> next{0};
    thread_local const std::size_t s = next.fetch_add(1, std::memory_order_relaxed) % kSlots;
    return s;
}
}  // namespace tally_detail

template <std::size_t N = 1>
class Tally {
public:
    void add(std::uint64_t n = 1, std::size_t i = 0) {
        slots_[tally_detail::slot()].v[i].fetch_add(n, std::memory_order_relaxed);
    }
    std::uint64_t load(std::size_t i = 0) const {
        std::uint64_t sum = 0;
        for (const Slot& s : slots_) sum += s.v[i].load(std::memory_order_relaxed);
        return sum;
    }

private:
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> v[N] = {};
    };
    Slot slots_[tally_detail::kSlots];
};
