# Rendering

## The problem

Bloodborne's renderer was written for the PS4's GPU, an AMD GCN part. The game
draws through FromSoftware's graphics layer, GX, which sits on Sony's Gnm
library: GX turns the engine's draws into PM4 command buffers - the packet
format the GPU's command processor reads - and the game ships its shaders as
GCN machine code. A PC has neither a GCN command processor nor a way to run
GCN shaders directly.

An emulator would answer this at the lowest level: decode every packet,
keep the GPU's registers, and reconstruct each draw from them. bbhost started
there and is moving up a level, to where the engine's intent is still visible.
Dark Souls III on PC, built on the same engine, puts Direct3D 11 under the
same GX layer; bbhost does the equivalent with Vulkan.

## How a frame is drawn today

**GX hooks.** bbhost hooks GX's draw, dispatch, resource-creation and state
methods. When the game draws, bbhost records the draw's inputs from the
engine's own objects - its shaders, vertex layout, state descriptions,
textures and constant buffers - and leaves a small token in the command
buffer. When the command processor reaches the token, the draw is built from
those inputs. No packet is decoded and no register file is consulted for a
draw.

**Compute dispatches** are tokens too. A dispatch through GX's compute
commit - the GPU particles, the tone-map and depth-of-field look-up tables -
takes its compute stage from the engine's objects at the call, as a draw does:
the compute shader, its views, UAVs, samplers and constant buffers, from which
the command processor builds the shader's user data, tables included. YEBIS's
passes and GX's own resource passes take the user data their writers set. A
dispatch the hooks did not take would still run from its packet and the
register file (`BBHOST_GX_NATIVE_DISPATCH=0` runs them all that way), and the
exit report counts those by reason.

**The command processor.** bbhost still walks the game's command buffers, for
what has not moved to tokens: labels and fences the game waits on, the
counters the GPU particles keep in GDS, occlusion queries and the constant
engine's tables. Clears and copies come as tokens from GX's own calls: a clear
names its view, and is made as the next pass's load op, or as a transfer clear
when something reads the image first.

**Recording.** The command processor leaves the Vulkan calls to a thread of
its own, `bb-record`. Each draw goes into a packet, and most of its other
commands - barriers, fills, copies and the like - into the same ordered
stream as typed records (`src/host/recorder.cpp`, `src/host/stream_ops.h`).
`bb-record` replays the stream into command buffers, begins and ends each one
and hands it to the thread that submits, while the command processor goes on
to the next draw. With `BBHOST_STREAM_SUBMIT=0` the command processor begins,
ends and submits the command buffers itself, to compare.

**Shaders.** GCN shaders are converted to SPIR-V when the game creates them
(the few that cannot be, at their first draw), so most pipelines are ready
before they are needed. There are two compilers:

- the *translator* (`src/gcn/translate.cpp`) reproduces the GCN program's
  execution faithfully - one invocation per lane, with the execution mask as
  data - and handles every shader the game uses;
- the *lifter* (`src/gcn/lift.cpp`) decompiles a shader into typed,
  structured SPIR-V, the form a person could read and edit. It is used for
  every pixel and compute shader, and every vertex shader on vertex input, it
  accepts - with the vertex formats read from the shader's parameters, or as
  specialisation constants, the way the translator reads them; the rest keep
  the translation. `tools/lift_verify.py` compares the two compilers' output
  byte for byte.

All 13,394 vertex, pixel and compute shaders in the game's bundles translate
to valid SPIR-V (`gcn2spv --check`). Domain, hull and local shaders run on the
host GPU's own tessellation stages; no geometry or export shader has been seen
in use. The lifter takes those too: the particles' local, hull and domain
shaders, whose one control point travels as stage attributes, and the game's
own hull draws (the Forbidden Woods' meshes, every hull in the bundles), whose
local, hull and domain shaders become one native pipeline - the LDS they pass
patches through turned into stage inputs and outputs, the game's patch list
drawn as it is, with no compute pass in front. `BBHOST_LIFT_TESS=0` keeps every
tessellation stage translated.

A compute shader is lifted against its dispatch's translation and binds
exactly what that binds - the parameter block with the page table, the images
and samplers, the particles' counter words - so a dispatch is set up the same
way whichever runs. Its memory reads and writes go through the same
page-table walk. GCN's loops that run until every lane's execution bit
clears become structured per-invocation loops, a block GCN branches over
where no lane needs it becomes a selection an invocation skips where its own
bit is clear, and the GPU particles' wave-wide counter idiom (`ds_append`/`ds_consume` in GDS, each lane's rank
from `v_mbcnt`) becomes one atomic add per invocation, so no compute lift
needs subgroup operations. A barrier is accepted only where every invocation
of the workgroup provably reaches it, and LDS, which no game compute shader
uses, is refused. `BBHOST_LIFT_COMPUTE=0` keeps every compute shader
translated.

**Textures and memory.** The game's memory is shared with the GPU, as on the
PS4: its GPU-visible parts (about 5 GB) are imported into Vulkan, because
shaders that cannot be fully resolved read memory through a page table. Textures are detiled on the GPU, and bbhost notices when the game
rewrites one by write-protecting its pages. Render targets are Vulkan images
sized from the engine's own views, and can be re-created at any resolution
while the game runs, the way the PC engine's GX device resizes.

**Native passes.** Several parts of the frame no longer run the game's GPU
path at all:

- YEBIS, the post-processing library (bloom, glare, depth of field, tone
  mapping): every stage's resources and constants are built by bbhost.
- Scaleform, the menu renderer: its draws are taken from its hardware
  abstraction layer.
- Depth snapshots, the on-screen overlay and FSR 1 upscaling are bbhost's
  own.

## Native or emulated

| Operation | Today |
|---|---|
| Draws | native: built from the engine's objects at a token |
| Tessellated draws | native: host tessellation stages; the game's hull draws one lifted pipeline each |
| Post-processing (YEBIS) | native resource building; the game's shaders |
| Menus (Scaleform) | native draw building from the HAL |
| Fills and copies without a shader | recognised and performed directly |
| Other compute dispatches | native: built at a token from the engine's objects (GX's compute stage) or the user data its writers set (YEBIS, GX's own passes); the game's shaders |
| Clears and target copies | native: tokens from GX's calls, in the view's terms |
| Labels and fences | emulated, on the command processor |
| Shaders | decompiled (lifted) to SPIR-V; a few refused programs translated (see below) |
| Resource identity | the game's memory addresses, plus write tracking |

## Shader lifting

Every shader the game creates goes through the lifter. One it refuses is translated instead, so nothing is lost. The program counts below cover every shader the game ships. The draw shares were measured at the twelve places `tools/area_check.py` visits; elsewhere they are expected, not measured.

| Shader stage | Programs lifted | Draws lifted (12 places) | Left |
|---|---|---|---|
| Pixel | 3,206 of 3,209 | 100% | FXAA3HQ, which needs a whole-wave proof; two debug shaders whose result depends on how the PS4 packs pixels into waves (no exact PC form). None is drawn at the 12 places. |
| Vertex | all 10,116 | 100% | - |
| Tessellation: local, hull, domain | all; the 141 mesh hull sets as native pipelines | 100% | - |
| Compute | all 38 in the bundles, and the 6 of the executable's own (GX's and YEBIS's) that the 12 places dispatch | 100% of dispatches | The executable carries 24 more, lifted when first dispatched; none is dispatched at the 12 places. |

A draw whose setup the lifter hasn't seen still falls back to the translator. The exit report counts those by reason, and `BBHOST_LIFT_TESS` and the other switches turn the lifted paths off for comparison.

| Step | State |
|---|---|
| Pixel and vertex shaders lifted | done |
| Tessellation lifted, mesh hulls native | done |
| Compute shaders lifted | done |
| HLSL: the lifted SPIR-V printed as HLSL and compiled with DXC to SPIR-V for Vulkan; checked byte-exact against the direct path before players switch | behind a switch, off by default (below) |
| Shaders as editable source, generated on the player's machine from their own game files (never shipped) | after HLSL |
| D3D12 (Shader Model 6) from the same HLSL, if ever wanted; D3D11 is not planned, since it lacks the quad operations 703 programs use | later, optional |

## HLSL

With `BBHOST_HLSL=1` every lift is printed as HLSL (`src/gcn/hlsl.cpp`) and
compiled by DXC to SPIR-V for Vulkan, and the stage takes DXC's module where
it declares the lift's interface and keeps its operations; anything else
keeps the lift's SPIR-V, counted by reason in the exit report. With
`BBHOST_HLSL=2` a stage whose module is not in the translation cache yet
keeps the lift for this start while two `bb-hlsl` threads compile it for the
next, so switching costs the game no stall. The switch is off by default
until the path is shown equal to the direct one in game.

- **What the HLSL is.** The lifter's SPIR-V stays the shader's IR of record:
  typed values in structured control flow. The printer reads it back and
  writes one statement an operation, values named by their SPIR-V ids,
  resources and stage variables by the engine's names, and constant-buffer
  loads annotated with the member they read
  (`// cbSceneParam.VC_MatrixViewProj[0].x`). Constant buffers are
  `ByteAddressBuffer`s read a dword at a time, as the lift reads them.
- **Exact by construction.** Every operation is spelled the way DXC lowers
  back to the lift's instruction: floats are `precise` (NoContraction); where
  HLSL's spelling is another instruction - `sign`, `ldexp`, `mad` (which DXC
  makes a fused multiply-add a driver must not split), `fmin`/`fmax`, the
  half packing, bitfield extracts - the operation is inline SPIR-V; the
  helper-lane query is the built-in, which keeps `discard` an `OpKill`; quad
  operations are `QuadReadLaneAt`/`QuadReadAcross*`; an invariant position
  (`BBHOST_VS_INVARIANT=1`) keeps its decoration. Each compiled module is
  checked against its lift: the same descriptor bindings, stage interface
  and specialization constants, and no float, image, quad or discard
  operation the lift does not have.
- **Tessellation and compute.** The LS is a vertex shader with an array
  output, the domain shader takes its control points as an `OutputPatch`,
  and the game's hull - one body each control point runs, a barrier, then the
  patch's factors - becomes HLSL's control-point and patch-constant
  functions from that one body. Compute lifts print too (raw addresses as
  `vk::RawBufferLoad`/`Store`, their counters as `InterlockedAdd` on a
  `vk::BufferPointer`).
- **Offline checks.** `gcn2hlsl` lifts every program in the bundles as the
  renderer does, prints, compiles and validates it: 13,318 of 13,318 vertex
  and pixel lifts, 450 of 450 tessellation stages, and the compute lifts;
  with `--runtime-offsets`, as on a device with maintenance8, 89 pixel
  shaders sample with a run-time offset, and those compile too.
  `hlsl_equiv` runs each lift and its DXC module on the same random inputs on
  lavapipe and compares the outputs bit for bit: 13,308 of the 13,318 vertex
  and pixel lifts identical, 10 differing only in the bits of NaNs (which
  Vulkan leaves to the implementation), none otherwise; the 89 with run-time
  offsets identical; all 450 tessellation stages identical (a domain shader
  behind a generated hull, a hull with its outputs stored from inside it,
  since random constants give factors that cull every patch).
  `tools/hlsl_verify.py` replays captured draws and dispatches with DXC's
  stage against the lift's.
- **DXC.** bbhost loads libdxcompiler (`dxcompiler.dll` on Windows) at run
  time, from beside the executable or the library path, and needs nothing
  else of it (not `dxil.dll`); without it the switch keeps every lift. A
  module costs DXC ~65 ms median (-O3, eight at a time on this machine), once:
  the result is kept in the translation cache. A sample's run-time offset,
  which Vulkan allows with maintenance8, is one thing DXC's own validator
  refuses with no option to allow it: such a module is compiled without that
  validator and validated by bbhost, with the offset allowed (a build without
  SPIRV-Tools keeps the lift).

## What comes next

- Resources identified by the engine's own objects and generations rather than
  by guest address and write tracking.
- Labels, fences and occlusion queries from GX's own calls, after which the PM4
  command processor only orders work.
- HLSL from the lifted shaders (the table above).

## Tools

| Tool | What it does |
|---|---|
| `gcn2spv` | translates and validates every shader in the game's bundles |
| `gcn2hlsl` | lifts every shader in the bundles, prints it as HLSL, compiles it with DXC and checks the module against the lift (`--tess` for the tessellation stages; `--spv` for one module) |
| `hlsl_equiv` | each lift and its DXC module on the same random inputs (on lavapipe), outputs compared bit for bit |
| `tools/hlsl_verify.py` | captured draws replayed with DXC's stage against the lift's |
| `gcndis` | disassembles GCN programs |
| F12 in game | dumps the frame, every render target and the draw list into a folder of its own (`tools/f12_check.py` reads it; `BBHOST_F12_PNG=1` for PNG) |
| `BBHOST_CAPTURE_DRAW` + `drawreplay` | captures one draw with its inputs and replays it outside the game; `BBHOST_CAPTURE_DISPATCH` does the same for a compute dispatch |
| `tools/area_check.py` | ten places in the game measured the same way every time - frame times, GPU time, draws by how their pixel shader ran, compiles, frames and lift checks - and two builds compared against their own noise ([area-check.md](area-check.md)) |
| `BBHOST_VK_VALIDATE=1` | runs with the Vulkan validation layer |
| `BBHOST_GPU_PROFILE=1` | per-pass GPU timings |
| `bbhost --stream-selftest[=N]` | checks the command stream headless against a CPU model, N rounds (16 by default), and exits with 0 when it matched; run it with `BBHOST_VK_VALIDATE=1` too |
