# Method, and the numbers everything else refers to

This directory describes the techniques DraStic (r2.5.2.2, AArch64 build) uses to
run Nintendo DS software at several times real time on cheap ARM handhelds. It is
a description of *techniques*, written from observation of the shipped binary and
from measurements of it running. No DraStic source exists and none is reproduced
here.

## Provenance

The debug-symbol build of DraStic that these notes are based on is freely
available and approved for distribution by its original developer, Exophase (who
has since gone quiet). Everything recorded here is reconstructible from that
binary plus a test device: the symbol names come out of the binary's own
`.symtab`, the code excerpts are `objdump` output, and every number is from a
run anyone can repeat with the commands given below.

That is the reason these notes live in the public tree rather than in private
research notes. They document *what the binary does* and *how fast it does it*.
They are a specification of behaviour, not a copy of an implementation, and
DSperate remains a reimplementation of the techniques rather than a port of the
code.

## How the observations were made

**Static.** The shipped `drastic` binary is an AArch64 PIE that was built with
`-g` and never stripped: it carries a full `.symtab` with ~3,200 named functions,
including the hand-written assembly kernels, which are emitted as size-0 `NOTYPE`
labels. Function names such as `cpu_translate_block`,
`video_3d_bin_polygons_1x` and `render_scanline_priority_encode_double_asm` are
therefore DraStic's own names, not names invented here. Two views were used:

- Ghidra decompilation for the C functions (2,677 functions exported).
- `objdump -d` for the hand-written assembly, where the decompiler is worse than
  useless — it models pinned registers as memory traffic and turns the dispatch
  loop into nonsense.

One mapping detail matters when cross-referencing: the Ghidra project is based at
`0x100000`, so a Ghidra address minus `0x100000` is the ELF virtual address used
by `objdump` and by the symbol table.

**Dynamic.** All timings below are from the target device — an Anbernic RG DS
(Rockchip RK3566: four Cortex-A55 cores at 1.992 GHz, ROCKNIX, Linux 7.0.2).
The A55 is **in-order**, which matters repeatedly: several things DraStic does
that look strange on paper are straightforwardly explained as scheduling for a
core that cannot reorder around a cache miss.

Two instruments were used:

- **DraStic's own ablation benchmark** (`--benchmark N`). It runs a warmup phase,
  a complete phase, and then one phase per subsystem with that subsystem disabled,
  and reports each subsystem's cost as the difference. Phase names come from
  `benchmark_phase_names` in the binary: Warmup, Complete, No Video 2D,
  No Video 3D, No Video Geometry, No Screen Update, No Audio.
- **`perf record`** on the device against the unstripped binary (same GNU build
  ID as the shipped one, so symbolisation is exact), sampling the Cortex-A55 PMU
  cycle counter.

Runs used `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`; the KMSDRM path
segfaults without a DRM master. That zeroes the Screen Update and Audio phases,
so those two rows below are not meaningful — everything else is.

## The numbers

### Ablation, Super Mario 64 DS, 300 frames, recompiler

| Phase | ms/frame |
|---|---|
| Complete | **2.98** |
| Video 2D | 1.96 |
| Video 3D | 1.08 |
| Video Geometry | 0.09 |
| Screen Update | 0.00 (dummy driver) |
| Audio | 0.00 (dummy driver) |

A DS frame is 16.74 ms. DraStic renders one in 2.98 ms — about **5.6× real
time** — on a 2 GHz in-order core.

Meteos, a 2D puzzle game, on the same device: **0.75 ms/frame**, 22× real time,
of which 0.21 ms is the 2D engine.

### The same scene, interpreted instead of recompiled

DraStic ships an interpreter (`--interpreter`) alongside the recompiler
(`--recompiler`, the default), which makes the recompiler's contribution
directly measurable rather than a matter of inference:

| | Complete | Video 2D | Video 3D | Geometry |
|---|---|---|---|---|
| Recompiler | **2.98 ms** | 1.96 | 1.08 | 0.09 |
| Interpreter | **10.64 ms** | 1.92 | 1.05 | 0.10 |

The rendering columns are unchanged, as they must be — the same pixels get drawn
either way — which is a useful check that the ablation is measuring what it
claims. The whole difference, 7.66 ms/frame, is CPU emulation. Frame time falls
**3.6×**; the CPU-emulation component itself falls by roughly an order of
magnitude, since under the recompiler the video subsystems account for very
nearly the entire frame.

### Where the cycles actually go

`perf` on the SM64DS benchmark run, 10,260 samples, bucketed by symbol:

| Bucket | Share of cycles |
|---|---|
| 3D rasteriser + resolve | 32.6 % |
| 2D engines | 25.2 % |
| **Translated guest code** (`[JIT]` region) | **14.7 %** |
| JIT support code (dispatch, block lookup, memory and I/O helpers) | 12.0 % |
| 3D geometry | 2.7 % |
| Scheduler / events | 1.7 % |
| SPU (all C, no assembly) | 1.1 % |
| Kernel, libc, startup, long tail | ~10 % |
| Texture cache | 0.04 % |

`perf` resolves the translation cache as its own DSO (`[JIT] tid …`), so the
14.7 % is measured, not estimated. A separate profile of a recorded gameplay
session (135,605 samples, `--input-playback`) puts the same figure at 17.4 %.

Three things in that table set up everything in the other documents:

1. **Emulating two ARM CPUs costs about a quarter of the machine.** Rendering
   costs nearly three-fifths. DraStic is not primarily a CPU emulator that also
   draws; it is a software renderer with a very cheap CPU emulator attached.
2. **Translation itself is free.** `cpu_block_create` is 0.20 % and
   `cpu_translate_*` does not appear at all. The recompiler is a
   translate-once-and-forget design, not a tiered or profiling one.
3. **The texture cache is 0.04 %.** That is not an accident, and
   [the 3D document](02-3d-software-rasteriser.md) explains what was done to it.

### Threading

Sample counts per thread on the benchmark run: 5,518 / 1,870 / 1,510 / 1,318 —
one main thread and three workers, on a four-core device. Call graphs show the
split cleanly:

- 2D engine A runs **on the main thread**, reached from the scheduler:
  `recompiler_entry_direct → execute_events → event_scanline_start_function →
  update_frame → video_render_scanlines → video_2d_render_scanlines →
  render_scanline → …`. Note the entry point is the *VBlank* scanline event:
  the whole frame is rendered in one batch there unless the guest changed
  2D state mid-frame, and engine B is rendered on a worker
  (`video_render_thread`) at the same time — see
  [04 §5](04-scheduler-deferral-and-memory.md).
- 3D runs **on the workers**, `video_3d_render_thread → video_3d_render_bins_1x`,
  with the main thread joining the same work via `update_frame_3d_1x`.

## The documents

- [01 — The ARM→AArch64 recompiler](01-arm-to-aarch64-jit.md)
- [02 — The 3D software rasteriser](02-3d-software-rasteriser.md)
- [03 — The 2D engines and the scanline pipeline](03-2d-engines-and-scanline-pipeline.md)
- [04 — The scheduler, deferred rendering, DMA and the memory system](04-scheduler-deferral-and-memory.md)
- [05 — The SPU and audio output](05-spu-and-audio-output.md)
- [06 — Game-specific behaviour](06-drastic-game-specific.md) — DraStic's
  per-game hack table and title-specific fixes, as a list to review

## Confidence

Claims below are drawn from disassembly of the shipped binary and are stated
plainly where the encoding is unambiguous (register assignments, table sizes,
emitted opcodes). Where something is an inference from surrounding code rather
than a direct reading, it says so. Measurements are from the runs described
above and are reproducible with the commands given.
