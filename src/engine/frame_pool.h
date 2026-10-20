#pragma once

// The frame loop's pooled object, borrowed and given back - as our source.
//
// The game freezes a few seconds into a world: two threads spin forever in
// `sub_1467f60` waiting for one of the six objects in the pool at
// `data_58b8788`, and the pool has all six taken with none free and nobody
// holding them. They were taken and then dropped, and the function
// that drops them is this one. It has three ways to lose an entry, and only
// the first needs two threads:
//
//  1. **The entry lives on the context, not the stack.** The acquire loop
//     stores its result into `ctx + 0x120` on *every* try, including the
//     failed ones, and the release reads the entry back from there:
//
//         while ((e = acquire(pool)) == 0) { ctx->0x120 = e; yield(); }
//         ...
//         release(pool, ctx->0x120);
//
//     Two threads on one context and the one that is spinning writes 0 over
//     the one that is working. `sub_1456990` frees by scanning the pool's
//     object array for the pointer it is given, so 0 matches nothing and it
//     quietly frees nothing at all.
//  2. **Two error paths abandon it.** If `sub_2565150` or `sub_2569c30`
//     returns non-zero the function jumps to its exit with the entry still
//     held and never releases it. No race needed.
//
// Ours keeps the entry on the stack where it belongs, so the release always
// gets the entry this call took, and gives it back on the error paths too.
// `ctx + 0x120` is still written and cleared, because that is the game's
// field and something else may read it - it is just no longer what we trust.
//
// BBHOST_POOL_FIX=0 runs the game's own function instead.

#include <cstdint>

struct ElfImage;

// Replaces guest 0x1467f60 when the eboot is the 1.09 build.
void frame_pool_install(ElfImage* image);

// For the exit report: how many borrows went through ours, how often another
// thread had overwritten the context's copy before the release (the race, as
// it happens rather than as a freeze), and how many entries the error paths
// would have dropped.
void frame_pool_report();
