#pragma once

// The translation cache (<data root>/bbhost/translation-cache.bin, beside the
// pipeline cache and off with it; BBHOST_TRANSLATION_CACHE=0 turns it off
// alone): what gcn::translate made of a program, kept between runs.
//
// Every start used to translate every shader GX creates again - ~1,350 pixel
// and ~1,660 vertex shaders, each twice (once for its resource paths, once for
// its stage) and then lifted, the stage manifest's few hundred and the compute
// shaders: 14 s of worker CPU in a world run on the Radeon 8060S (translate
// and lift 9.9 s + 3.8 s, libraries 0.2 s - the driver's pipeline cache had
// already made the compiles free; cpu.md CPU-13). The translation is a pure
// function of the program, its TranslateOptions and a few process-wide
// switches (gcn::translation_switches), and the lift one of the same and the
// translation, so a start can take an earlier one's results instead: keyed on
// all three, and on the translator's own sources (src/gcn,
// cmake/translator_fingerprint.cmake), so a build with another translator or
// lifter starts the file again. Kyo's preload keeps its SPIR-V the same way.
//
// Entries are compressed (zlib) and read lazily, at a lookup; the file is read
// whole at device start, rewritten whole (to a temporary a chunk at a time,
// then renamed) once 4 MiB or 2% of new translations have come in - at most
// once a minute - and at exit.

#include "gcn/lift.h"
#include "gcn/translate.h"

#include <string>

namespace gpu {

// What a translation is for. kPaths: read for its bindings only (paths_for and
// the resource paths a compile at creation offers it), so its entry keeps no
// SPIR-V and the result has none, taken from the cache or not. The lifts'
// entries are kept under uses of their own (lift_cached).
enum class TranslationUse : std::uint8_t { kStage = 0, kPaths = 1, kLiftPixel = 2, kLiftVertex = 3, kLiftLs = 4, kLiftDomain = 5, kLiftHull = 6,
                                           kLiftCompute = 7, kHlsl = 8 };

// gcn::translate through the cache.
gcn::TranslateResult translate_cached(const gcn::Program& prog, const gcn::TranslateOptions& o,
                                      TranslationUse use = TranslationUse::kStage);
// gcn::lift_pixel_shader (`vertex` false) or gcn::lift_vertex_shader through
// the cache. `reference` must be translate_cached(prog, o)'s result, as every
// lift's is: the entry is keyed on the program and options alone. A lift taken
// from the cache has its SPIR-V and rejections, not its proof or listing,
// which no caller reads.
gcn::LiftResult lift_cached(bool vertex, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference);
// gcn::lift_compute_shader through the cache, the same way: `reference` is
// translate_cached(prog, o)'s result, the dispatch's translation.
gcn::LiftResult lift_compute_cached(const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference);
// A tessellation stage's lift (gcn::lift_ls_shader for kLiftLs,
// gcn::lift_hull_shader for kLiftHull, gcn::lift_domain_shader for
// kLiftDomain) the same way, keyed on its TessLift too.
gcn::LiftResult lift_tess_cached(TranslationUse use, const gcn::Program& prog, const gcn::TranslateOptions& o, const gcn::TranslateResult& reference,
                                 const gcn::TessLift& tess);

// BBHOST_HLSL=1 (off by default, while the path is checked against the direct
// one): every lift lift_cached, lift_compute_cached and lift_tess_cached return
// is printed as HLSL (gcn/hlsl.h) and compiled by DXC (host/dxc.h,
// libdxcompiler loaded at run time), and the stage takes DXC's SPIR-V in place
// of the lifter's when it declares the same interface and keeps the lift's
// operations (gcn::hlsl_interface_diff, hlsl_operation_diff). Anything else -
// DXC not found, a module the printer does not take, a compile error, a
// difference - keeps the lifter's SPIR-V and is counted by reason in the exit
// report. The result is kept in this cache under its own use (kHlsl), keyed on
// the lifted module, DXC's version and its arguments: a later start compiles
// nothing again. BBHOST_HLSL_O=0..3 sets DXC's optimization level (default 3),
// BBHOST_HLSL_DXC the library to load, BBHOST_HLSL_DUMP=<dir> writes each
// compiled module's HLSL there (made from the game's shaders: it stays on this
// machine). BBHOST_HLSL=2 compiles behind the game instead: a stage whose
// module the cache does not have keeps the lifter's SPIR-V for this start, and
// two bb-hlsl threads (BBHOST_HLSL_THREADS) compile it for the next - the first
// start after switching costs nothing, the next takes every module it met from
// the cache.
bool hlsl_on();
// BBHOST_HLSL: whether `module` is DXC's module of a lift - one the switch gave
// a stage in the lift's place; the engine's names put on it afterwards leave
// it the same module - and if so, the lift's own SPIR-V in `lift`. A driver
// that refuses a pipeline of DXC's modules gets it built from the lifts'
// instead (render.cpp create_gfx_pipeline, gpu.cpp's compute pipelines): the
// draw or dispatch is never skipped for it.
bool hlsl_lift_of(const std::vector<std::uint32_t>& module, std::vector<std::uint32_t>& lift);
// Such a refusal (`what` was being built), with the number of stages built
// from their lifts instead and whether the driver took that; counted by
// `what` in the exit report's HLSL line.
void hlsl_note_refused(const char* what, int stages, bool rebuilt);
// Stages a later pipeline gave their lift's module before its first attempt,
// their DXC module refused in an earlier one; counted in the same line.
void hlsl_note_lift_again(int stages);

// At device start, after the device's features are given to the translator
// (they are part of every key).
void translation_cache_load(const std::string& path);
// The whole file, when translations were added since the last write; `force`
// writes now (the exit), otherwise once 4 MiB or 2% of it is new, at most once
// a minute.
void translation_cache_save(const std::string& path, bool force);
// The same on a thread of its own (the 300-flip report's).
void translation_cache_save_async(const std::string& path);
// The exit report's line.
void translation_cache_report();
// Since the last call: "translations N cached (x ms), M translated (y ms)", or
// empty when there were none (the 300-flip report).
std::string translation_cache_window();
// The translator's fingerprint (cmake/translator_fingerprint.cmake), which the
// shader caches are stamped with: another one starts them all again.
const char* translator_fingerprint();
// The calling thread's translations and lifts join the cache only while it
// holds less than its byte limit, so they never push out what the game used:
// the first start's compile of every shader in the game's bundles (render.cpp,
// the shader sweep). Their count left out, for its report.
void translation_cache_bounded(bool on);
std::uint64_t translation_cache_bounded_left_out();
// This start had no translations to take: the cache was empty, unreadable or
// another translator's, so every shader is translated again. False with the
// cache off, and before it is loaded.
bool translation_cache_cold();

}  // namespace gpu
