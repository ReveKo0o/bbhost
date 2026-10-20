#pragma once

// Host implementation of the Orbis pthread object model.
//
// Every Orbis handle (ScePthreadMutex, ScePthreadCond, ScePthreadRwlock,
// ScePthread, the attr types, the libc pthread_* aliases) is a pointer-sized
// slot in guest memory that holds a pointer to an opaque object. Init writes
// the pointer, destroy clears it, and a NULL slot is the FreeBSD static
// initializer (lazily initialised on first use). Objects live in a low arena
// (below 2^40) because the engine packs owner ids into 40 bits.
//
// All entry points return FreeBSD errno values (0 on success). Deadlines are
// optional; nullptr means block forever.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace gsync {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

// FreeBSD errno values.
constexpr int kEPERM = 1;
constexpr int kEDEADLK = 11;
constexpr int kEBUSY = 16;
constexpr int kEINVAL = 22;
constexpr int kEAGAIN = 35;
constexpr int kETIMEDOUT = 60;

// Orbis/FreeBSD mutex types.
constexpr int kMutexErrorCheck = 1;
constexpr int kMutexRecursive = 2;
constexpr int kMutexNormal = 3;
constexpr int kMutexAdaptive = 4;
constexpr int kMutexDefault = kMutexErrorCheck;

struct Thread;

// Low arena for handle objects.
void* arena_alloc(std::size_t n);
void arena_free(void* p, std::size_t n);
bool arena_owns(const void* p);

// Current thread's handle object (created lazily for host-born threads).
Thread* current_thread();
Thread* thread_new();
void thread_free(Thread* t);
void set_current_thread(Thread* t);

// Mutex.
int mutex_init(void** slot, int type);
int mutex_destroy(void** slot);
int mutex_lock(void** slot, const Deadline* dl);
int mutex_trylock(void** slot);
int mutex_unlock(void** slot);
// The uncontended cases alone, for the guest's calls without the HLE thunk:
// no host TLS (self comes from the caller), no lock, no allocation. True when
// done; false leaves everything to mutex_lock / mutex_unlock (a static
// initializer, contention, a sleeper to wake, an error to report).
bool mutex_lock_fast(void** slot, Thread* self);
bool mutex_unlock_fast(void** slot, Thread* self);

// Mutex attr.
int mutexattr_init(void** slot);
int mutexattr_destroy(void** slot);
int mutexattr_settype(void** slot, int type);
int mutexattr_gettype(void** slot, int* type);
int mutexattr_type(void** slot);  // kMutexDefault when slot is null/invalid

// Condition variable.
int cond_init(void** slot);
int cond_destroy(void** slot);
int cond_wait(void** cond_slot, void** mutex_slot, const Deadline* dl);
int cond_signal(void** slot, bool all);

// Read/write lock.
int rw_init(void** slot);
int rw_destroy(void** slot);
int rw_rdlock(void** slot, const Deadline* dl);
int rw_wrlock(void** slot, const Deadline* dl);
int rw_tryrdlock(void** slot);
int rw_trywrlock(void** slot);
int rw_unlock(void** slot);
// Without the thunk (hle/pthread.cpp's raw entries): a read lock while no
// writer holds it, true when taken; and a reader's unlock, 0 done, 1 done
// with sleepers to wake (rw_wake_slot, from a thunked entry), -1 for the
// slow path (a writer's unlock, an error).
bool rw_rdlock_fast(void** slot);
int rw_unlock_fast(void** slot);
void rw_wake_slot(void** slot);

// How often the slow paths ran since the start: mutex locks that had to
// wait, condition waits and signals that found a waiter, read/write locks
// that had to wait (the 300-flip report's timing line).
struct SyncCounts {
    std::uint64_t mutex_contended, cond_waits, cond_signals, rw_sleeps;
};
SyncCounts counts();

// Deadline helpers.
inline Deadline after_us(std::uint64_t us) {
    return Clock::now() + std::chrono::microseconds(us);
}
// FreeBSD timespec (CLOCK_REALTIME absolute) -> steady deadline.
Deadline from_realtime_abs(std::int64_t sec, std::int64_t nsec);

}  // namespace gsync
