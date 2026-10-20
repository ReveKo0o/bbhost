// Phase 0.5: Orbis sync semantics over hle/sync.cpp.
#include "hle/sync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
int g_fail = 0;
#define CHECK(c)                                                             \
    do {                                                                     \
        if (!(c)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)
using namespace gsync;
}  // namespace

int main() {
    // Slot model: init writes a pointer below 2^40; destroy clears it.
    void* m = nullptr;
    CHECK(mutex_init(&m, kMutexRecursive) == 0);
    CHECK(m != nullptr && reinterpret_cast<std::uintptr_t>(m) < (1ull << 40));
    CHECK(arena_owns(m));
    void* copy = m;  // handles copy by value
    CHECK(mutex_lock(&m, nullptr) == 0);
    CHECK(mutex_lock(&copy, nullptr) == 0);  // recursive through the copy
    CHECK(mutex_unlock(&copy) == 0);
    CHECK(mutex_unlock(&m) == 0);
    CHECK(mutex_destroy(&m) == 0);
    CHECK(m == nullptr);

    // Error-check default and self-deadlock detection.
    void* e = nullptr;
    CHECK(mutex_lock(&e, nullptr) == 0);  // NULL = static initializer
    CHECK(mutex_lock(&e, nullptr) == kEDEADLK);
    CHECK(mutex_trylock(&e) == kEBUSY);
    CHECK(mutex_unlock(&e) == 0);
    CHECK(mutex_unlock(&e) == kEPERM);

    // Garbage slot is rejected, not dereferenced.
    void* g = reinterpret_cast<void*>(0x1234);
    CHECK(mutex_lock(&g, nullptr) == kEINVAL);

    // Timed lock times out while another thread holds it.
    void* t = nullptr;
    CHECK(mutex_init(&t, kMutexNormal) == 0);
    std::atomic<bool> held{false};
    std::thread holder([&] {
        mutex_lock(&t, nullptr);
        held = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        mutex_unlock(&t);
    });
    while (!held) {
    }
    Deadline dl = after_us(20000);
    const auto t0 = Clock::now();
    CHECK(mutex_lock(&t, &dl) == kETIMEDOUT);
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(140));
    CHECK(mutex_lock(&t, nullptr) == 0);
    holder.join();
    CHECK(mutex_unlock(&t) == 0);

    // Cond wait fully releases a recursive mutex and restores its depth.
    void* cm = nullptr;
    void* cv = nullptr;
    CHECK(mutex_init(&cm, kMutexRecursive) == 0);
    CHECK(cond_init(&cv) == 0);
    CHECK(mutex_lock(&cm, nullptr) == 0);
    CHECK(mutex_lock(&cm, nullptr) == 0);  // depth 2
    std::atomic<int> stage{0};
    std::thread signaler([&] {
        CHECK(mutex_lock(&cm, nullptr) == 0);  // must succeed while waiter sleeps
        stage = 1;
        CHECK(cond_signal(&cv, false) == 0);
        CHECK(mutex_unlock(&cm) == 0);
    });
    CHECK(cond_wait(&cv, &cm, nullptr) == 0);
    CHECK(stage == 1);
    CHECK(mutex_unlock(&cm) == 0);
    CHECK(mutex_unlock(&cm) == 0);  // depth restored to 2
    CHECK(mutex_unlock(&cm) == kEPERM);
    signaler.join();

    // Timed cond wait.
    CHECK(mutex_lock(&cm, nullptr) == 0);
    dl = after_us(10000);
    CHECK(cond_wait(&cv, &cm, &dl) == kETIMEDOUT);
    // Under a millisecond: never before the deadline, and not
    // rounded up to a timer tick either - the median of twenty 400 us waits
    // well under the 15.6 ms (or 1 ms) tick a whole-millisecond wait takes.
    {
        std::vector<long long> took;
        for (int i = 0; i < 20; ++i) {
            const auto s0 = Clock::now();
            Deadline sub = after_us(400);
            CHECK(cond_wait(&cv, &cm, &sub) == kETIMEDOUT);
            CHECK(Clock::now() >= sub);
            took.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s0).count());
        }
        std::sort(took.begin(), took.end());
        CHECK(took[10] >= 400 && took[10] < 5000);
        if (std::getenv("SYNC_TEST_VERBOSE")) {
            std::printf("400 us timed cond waits: min %lld, median %lld, max %lld us\n", took.front(), took[10], took.back());
        }
    }
    CHECK(mutex_unlock(&cm) == 0);
    CHECK(cond_destroy(&cv) == 0);
    CHECK(mutex_destroy(&cm) == 0);

    // Producer and consumers over cond_signal: every item is taken and no
    // wakeup is lost (a lost one leaves a consumer asleep with items queued
    // until its deadline, which the check counts instead of hanging).
    {
        void* qm = nullptr;
        void* qc = nullptr;
        CHECK(mutex_init(&qm, kMutexNormal) == 0);
        CHECK(cond_init(&qc) == 0);
        int queued = 0, taken = 0;
        bool closing = false;
        std::atomic<int> stalls{0};
        constexpr int kItems = 20000;
        std::vector<std::thread> consumers;
        for (int c = 0; c < 4; ++c) {
            consumers.emplace_back([&] {
                CHECK(mutex_lock(&qm, nullptr) == 0);
                for (;;) {
                    while (queued == 0 && !closing) {
                        Deadline d = after_us(2000000);
                        if (cond_wait(&qc, &qm, &d) == kETIMEDOUT) stalls.fetch_add(1);
                    }
                    if (queued == 0) break;
                    --queued;
                    ++taken;
                }
                CHECK(mutex_unlock(&qm) == 0);
            });
        }
        for (int i = 0; i < kItems; ++i) {
            CHECK(mutex_lock(&qm, nullptr) == 0);
            ++queued;
            CHECK(cond_signal(&qc, false) == 0);
            CHECK(mutex_unlock(&qm) == 0);
            if ((i & 63) == 0) std::this_thread::yield();
        }
        CHECK(mutex_lock(&qm, nullptr) == 0);
        closing = true;
        CHECK(cond_signal(&qc, true) == 0);
        CHECK(mutex_unlock(&qm) == 0);
        for (auto& c : consumers) c.join();
        CHECK(taken == kItems);
        CHECK(stalls == 0);
        CHECK(cond_destroy(&qc) == 0);
        CHECK(mutex_destroy(&qm) == 0);
    }

    // Rwlock: two readers coexist, writer excludes.
    void* rw = nullptr;
    CHECK(rw_init(&rw) == 0);
    CHECK(rw_rdlock(&rw, nullptr) == 0);
    std::atomic<int> r2{0};
    std::thread reader([&] {
        r2 = rw_rdlock(&rw, nullptr) == 0 ? 1 : -1;
        rw_unlock(&rw);
    });
    reader.join();
    CHECK(r2 == 1);
    CHECK(rw_trywrlock(&rw) == kEBUSY);
    CHECK(rw_unlock(&rw) == 0);
    CHECK(rw_wrlock(&rw, nullptr) == 0);
    CHECK(rw_tryrdlock(&rw) == kEBUSY || rw_tryrdlock(&rw) == kEDEADLK);
    std::thread wtimed([&] {
        Deadline d2 = after_us(10000);
        CHECK(rw_rdlock(&rw, &d2) == kETIMEDOUT);
    });
    wtimed.join();
    CHECK(rw_unlock(&rw) == 0);
    CHECK(rw_destroy(&rw) == 0);

    // Rwlock under load: readers on the fast paths as the raw entries run
    // them (and the slow ones when those decline), writers on the slow ones.
    // A writer never overlaps a reader or another writer, and nothing is lost.
    void* srw = nullptr;
    CHECK(rw_init(&srw) == 0);
    std::atomic<int> readers_in{0}, writers_in{0}, overlap{0};
    std::atomic<long> reads{0}, writes{0};
    std::vector<std::thread> rws;
    for (int t = 0; t < 6; ++t) {
        rws.emplace_back([&] {
            for (int i = 0; i < 50000; ++i) {
                if (!rw_rdlock_fast(&srw)) CHECK(rw_rdlock(&srw, nullptr) == 0);
                readers_in.fetch_add(1);
                if (writers_in.load()) overlap.fetch_add(1);
                readers_in.fetch_sub(1);
                const int r = rw_unlock_fast(&srw);
                if (r < 0) CHECK(rw_unlock(&srw) == 0);
                else if (r > 0) rw_wake_slot(&srw);
                reads.fetch_add(1);
                std::this_thread::yield();  // reader-preferring: let the writers in
            }
        });
    }
    for (int t = 0; t < 2; ++t) {
        rws.emplace_back([&] {
            for (int i = 0; i < 5000; ++i) {
                CHECK(rw_wrlock(&srw, nullptr) == 0);
                if (writers_in.fetch_add(1) != 0 || readers_in.load() != 0) overlap.fetch_add(1);
                writers_in.fetch_sub(1);
                CHECK(rw_unlock(&srw) == 0);
                writes.fetch_add(1);
            }
        });
    }
    for (auto& t : rws) t.join();
    CHECK(overlap == 0);
    CHECK(reads == 300000);
    CHECK(writes == 10000);
    CHECK(rw_unlock_fast(&srw) == -1);  // nothing held
    CHECK(rw_destroy(&srw) == 0);

    // Destroy while locked is EBUSY.
    void* b = nullptr;
    CHECK(mutex_init(&b, kMutexDefault) == 0);
    CHECK(mutex_lock(&b, nullptr) == 0);
    CHECK(mutex_destroy(&b) == kEBUSY);
    CHECK(mutex_unlock(&b) == 0);
    CHECK(mutex_destroy(&b) == 0);

    // The guest's raw entries: the fast paths take and release a free mutex,
    // hand anything else back, and mix with the slow paths under contention.
    void* f = nullptr;
    CHECK(mutex_init(&f, kMutexNormal) == 0);
    Thread* me = current_thread();
    CHECK(!mutex_lock_fast(&f, nullptr));  // no thread from the TCB yet
    CHECK(mutex_lock_fast(&f, me));
    CHECK(!mutex_lock_fast(&f, me));  // relocking a normal mutex: the slow path reports it
    CHECK(mutex_trylock(&f) == kEBUSY);
    CHECK(mutex_unlock_fast(&f, me));
    CHECK(!mutex_unlock_fast(&f, me));  // not held
    void* none = nullptr;
    CHECK(!mutex_lock_fast(&none, me));  // a static initializer is the slow path's
    void* fr = nullptr;
    CHECK(mutex_init(&fr, kMutexRecursive) == 0);
    CHECK(mutex_lock_fast(&fr, me) && mutex_lock_fast(&fr, me) && mutex_lock(&fr, nullptr) == 0);
    CHECK(mutex_unlock_fast(&fr, me) && mutex_unlock(&fr) == 0 && mutex_unlock_fast(&fr, me));
    CHECK(mutex_unlock(&fr) == kEPERM);
    std::uint64_t counter = 0;
    std::atomic<int> inside{0}, overlaps{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < 6; ++w) {
        workers.emplace_back([&, w] {
            Thread* self = current_thread();
            for (int i = 0; i < 100000; ++i) {
                const bool fast = ((i + w) % 3) != 0;  // two in three calls raw, as the guest's mostly are
                if (!(fast && mutex_lock_fast(&f, self))) CHECK(mutex_lock(&f, nullptr) == 0);
                if (inside.fetch_add(1) != 0) overlaps.fetch_add(1);
                ++counter;
                inside.fetch_sub(1);
                if (!(fast && mutex_unlock_fast(&f, self))) CHECK(mutex_unlock(&f) == 0);
            }
        });
    }
    for (auto& w : workers) w.join();
    CHECK(overlaps == 0);
    CHECK(counter == 600000);
    CHECK(mutex_destroy(&f) == 0);
    CHECK(mutex_destroy(&fr) == 0);

    // Mutex attr.
    void* a = nullptr;
    CHECK(mutexattr_init(&a) == 0);
    CHECK(mutexattr_settype(&a, kMutexRecursive) == 0);
    CHECK(mutexattr_type(&a) == kMutexRecursive);
    CHECK(mutexattr_destroy(&a) == 0);
    CHECK(mutexattr_type(&a) == kMutexDefault);

    if (g_fail) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::puts("sync_test ok");
    return 0;
}
