# The scheduler, deferred rendering, DMA and the memory system

The first three documents cover where the cycles go. This one covers the
machinery that *decides when* things run — and it turns out that most of what
makes DraStic fast outside the kernels is a single policy applied everywhere:
**do nothing at the moment the guest asks for it; record that it asked, and do
the work once, later, at the coarsest point where the result is still
observable.**

It applies to interrupts, timers, DMA, 2D register writes, palette and OAM
stores, VRAM remaps, the 3D command stream and (in [05](05-spu-and-audio-output.md))
the mixer. The scheduler itself is 1.7 % of cycles; the deferral is what keeps
everything it schedules cheap.

Instruction counts are from the qemu harness (SM64DS, 4,205 frames, exact
guest-instruction counts):

| Function | Per frame | Share |
|---|---|---|
| `process_geometry_commands` | 130,867 | 2.01 % |
| `geometry_transform_vertexes` (+ `_block_asm`) | 117,145 + 38,281 | 2.39 % |
| `geometry_flush_polygons` | 73,447 | 1.13 % |
| `execute_events` | 35,557 | 0.55 % |
| `event_scanline_start_function` | 23,909 | 0.37 % |
| `event_hblank_start_function` | 18,575 | 0.29 % |
| `event_force_task_switch_function` | 17,820 | 0.27 % |
| `dma_transfer` + `dma_transfer_gxfifo` | 7,696 + 5,651 | 0.21 % |
| `store_io_register_arm7_16` | 8,025 | 0.12 % |
| `remap_*` (VRAM/WRAM/ITCM/palette/OAM) | ~360 | 0.00 % |
| `memory_check_code_region` | 12 | 0.00 % |

---

## 1. The event list is delta-encoded, and both CPUs run to the next event

`schedule_event` / `remove_event` / `execute_events` implement a sorted
doubly-linked list of **fixed event slots** — there is no allocation. Slots
are assigned by `initialize_event_list` and by convention elsewhere:

| Slot | Event |
|---|---|
| 0 | HBlank start |
| 1 | Scanline start |
| 2 | Forced task switch (CPU sync) |
| 3–6 | ARM7 timers 0–3 overflow |
| 7–10 | ARM9 timers 0–3 overflow |
| 11 | Gamecard IRQ |
| 12+ch | DMA channel completion |

Each entry's `trigger_cycles` is stored **relative to its predecessor**, so
the head's value is always "cycles until the next event" and inserting an
event is a walk that subtracts as it goes:

```
schedule_event(cycles, idx):
    for e in list:
        if cycles <= e.delta:  insert before e; e.delta -= cycles; return
        cycles -= e.delta
    append
```

`execute_events` then needs no comparisons against absolute time:

```
system->cycles += slice
if (slice < head.delta) head.delta -= slice
else run head; then keep running while the new head's delta == 0
```

The timeslice is *defined* as the head's delta:
`system->slice_cycles = *events.pending` in `recompiler_entry` and
`cpu_next_action_arm7_to_event_update`. The ARM9 runs the whole slice, then
the ARM7 runs the *same* slice (`cpu_next_action_arm9_to_arm7` just adds
`slice_cycles` to the ARM7's budget and jumps in), then events fire. Both
CPUs count in ARM9 cycles — the ARM7 path doubles each instruction's cost,
as noted in [01 §2](01-arm-to-aarch64-jit.md) — so one clock, one slice, two
CPUs, zero per-instruction synchronisation.

The scanline is two events: scanline-start schedules HBlank at `+0xc00`
(3,072 cycles) and HBlank schedules the next scanline-start at `+0x4a4`
(1,188 cycles); 4,260 ARM9 cycles per line, 27.9 % of it HBlank, matching
the hardware. So in the steady state a CPU slice is 3,072 or 1,188 cycles,
and `recompiler_entry`'s loop runs 526 times a frame.

**Fine-grained interleaving only on demand.** When one CPU halts, wakes the
other, or touches the IPC, `event_force_task_switch_function` schedules slot
2 at the *next 128-cycle boundary* (`0x80 - (cycles & 0x7f)`). That
truncates the current slice so the other CPU gets to run almost immediately —
17,820 instructions a frame in SM64DS, so it fires often — but only when a
cross-CPU dependency exists. The rest of the time the CPUs run whole
scanlines uninterrupted.

---

## 2. Timers are derived on read, not counted

There is no per-tick timer update anywhere. A running timer stores the cycle
count at which it started (`timer->start_cycles = system->cycles` in
`event_timer_overflow_function`) and its reload value; the count is computed
when the guest reads it:

```
memory_timer_count:
    if (!running) return reload
    return ((now - start) >> prescale) + reload      ; & 0xffff
```

where `now` includes the current slice and the CPU's private cycle
adjustment. Overflow is simply an event in slots 3–10 scheduled
`reload_period << prescale` cycles out; the handler raises the IRQ bit,
advances a cascaded timer if `count-up` is set, and reschedules itself. Four
timers per CPU cost nothing unless the guest reads them or they overflow.

---

## 3. Interrupts are evaluated at the source

Every place that sets an `IF` bit — VBlank, HBlank, VCount match, timers, DMA
completion, IPC, gamecard — does the same three lines:

```
IF |= bit
if (!(cpu->halt_state & 6))
    cpu->irq_pending = -IME & IF & IE
```

`irq_pending` is a precomputed "an interrupt should be taken" word. The CPU
never recomputes `IME & IE & IF`; the recompiled code never looks at it at
all. It is consumed in exactly two places:

- `recompiler_entry` / `cpu_next_action_*`, between slices, which is where
  most interrupts are actually taken — at the event that raised them.
- `cpu_alerts_check_interrupts`, a thunk selected through `pending_actions`
  (bit 1 = check interrupts, bit 2 = DMA modified code, `cpu_alerts_jump_table`
  picks the combination), which is reached from a block boundary when
  something *inside* a slice raised an interrupt or wrote code. It spills the
  pinned guest registers, recovers the guest PC from the block's metadata via
  `cpu_translate_get_pc`, calls `execute_arm_raise_interrupt`, and re-enters.

`cpu_alerts_check_interrupts` is 7,409 instructions a frame in SM64DS;
`cpu_alerts_none` (the "nothing pending" thunk) 696. Interrupt latency is
therefore bounded by the block, not the instruction, and the cost of checking
is zero in translated code.

---

## 4. DMA is one copy, one bitmap test, one event

`dma_transfer` performs the **entire transfer immediately**, on the CPU that
started it, as a memory copy — there is no per-word scheduling. What makes it
cheap and safe is what surrounds the copy:

**Regions, not pages.** DMA does not go through the 2 KB page table. Each CPU
has a **16-entry region table indexed by `address >> 23`** (8 MB regions) with
an `address_mask`, a `type` (0 = direct host pointer, 1 = pointer-returning
function, 2 = per-halfword function) and `load8/16/32` / `store8/16/32`
handlers. A main-RAM→VRAM copy resolves both ends once per region span and
copies with a halfword loop; only the I/O-type regions go through per-access
functions.

**Timing from a table.** The transfer's cycle cost is
`count × cycles_[non]sequential[cpu][width][src_region][dst_region]`, from two
static tables, multiplied by the per-game DMA adjustment — and the completion
IRQ is a scheduled event (slot 12 + channel), not an immediate one. Games
that poll `DMAxCNT.busy` or wait for the DMA IRQ see plausible timing; the
copy itself has already happened.

**The code bitmap.** Each region can carry two bitmaps of translated-code
presence: a coarse one with one bit per 64 KB and a fine one with one bit per
2 KB. After a copy, `memory_check_code_region` ORs the bits covering the
destination range (auto-vectorised into 16-byte OR chunks for long ranges) —
12 instructions a frame in SM64DS. Only if the result is non-zero does it set
`pending_actions |= 4`, and the `cpu_alerts_dma_modified_code` thunk flushes
the translation cache at the next block boundary. This is the DMA-side
equivalent of the recompiler's three SMC filters: overlays DMA'd into
main RAM cost one bitmap OR unless they land on live code.

**HBlank DMA to VRAM forces a render catch-up.** In
`event_hblank_start_function`, before servicing an HBlank-triggered channel
whose destination is in `0x06xxxxxx`, DraStic calls
`video_render_scanlines(current_line)`. That is the coupling between DMA and
the lazy 2D pipeline in §5: the renderer must consume VRAM as it *was* before
the guest's per-line DMA overwrites it.

GXFIFO DMA (`dma_transfer_gxfifo`) is separate: it feeds the geometry
command queue (§6) directly from the source region with
`queue_geometry_command_packed_multi`, optionally charging the geometry
engine's own per-command cycle cost, and reschedules itself while the FIFO
would still be draining.

---

## 5. 2D is rendered lazily, with mid-frame writes journaled

[03 §1](03-2d-engines-and-scanline-pipeline.md) describes the per-scanline
pipeline. The pipeline is per-scanline; the *scheduling* of it is not. In
the common case **the whole frame is rendered in one call at VBlank**:

```
update_frame:                                  ; line 192
    video_render_scanlines(video, 0xbf)
        if (scanline == 0 && line == 0xbf):    ; nothing rendered yet this frame
            signal video_render_thread         ; engine B, lines 0..191, on a worker
            video_2d_render_scanlines(engine_a, 0, 0xbf)   ; engine A, on this thread
            wait for the worker
```

Engine B goes to `video_render_thread` and engine A stays on the main
thread, so the two 2D engines run in parallel — which the "2D on the main
thread" call graph in [00](00-method-and-measurements.md) only half shows.

Rendering earlier than VBlank happens only when something *would* change the
output mid-frame. `event_hblank_start_function` renders up to the current
line if the guest has written 2D state since the last render (a per-frame
"dirty" flag, or the display-capture bit in `DISPCNT`), and before an
HBlank DMA into VRAM (§4). Otherwise it does nothing on 192 of 192 lines.

**But raster effects still work**, because writes are journaled, not
applied. The I/O store handlers, the palette store handlers and the OAM store
handlers call `video_2d_queue_event(engine, address, value, size, scanline)`
into a **32,768-entry × 12-byte journal** per engine. `video_2d_render_scanlines`
walks lines from `start` to `end` and, after rendering each line, replays
every journal entry stamped with that scanline — `DISPCNT`, `BGxCNT`, scroll,
affine parameters, windows, blend registers, `MASTER_BRIGHT`, palette and OAM
bytes — through `video_2d_process_event` before rendering the next line. A
game that writes a scroll register in every HBlank gets 192 journal entries
and a render that sees each of them on the right line; a game that writes
nothing mid-frame gets a single flat render.

**Palette and OAM are shadowed copy-on-first-write.** `store_palette_deferred_first16`
shows the mechanism: the first palette write of a frame `memcpy`s the 2 KB
palette to a shadow, retargets the palette *region's* handlers to
`store_palette_deferred8/16/32`, and — only if the new value differs from the
old — journals the write and updates the shadow. The guest reads its own
writes back from the shadow; the renderer keeps reading the frame-start
palette until it replays the journal. Silent-store elimination again, and
`remap_palette_oam_deferred` / `_direct` flip the handlers back at frame
boundaries (75–93 instructions a frame).

**VRAM bank remaps are deferred to render time too.** A `VRAMCNT` write sets
a bit in `video->page_dirty[]` / `page_dirty_mask`; `video_render_scanlines`
applies the pending `remap_address_region_vram` calls at the top of its next
run. A game that toggles bank mappings several times a frame — common in
capture-based effects — pays for one remap.

For a reimplementation this is the important correction to the "2D is
synchronous per scanline" framing: it is *semantically* synchronous, and
*executed* in one batch whenever the guest lets it be.

---

## 6. The 3D command stream is a log replayed at frame end

The GXFIFO fast path in [01 §8](01-arm-to-aarch64-jit.md) does not execute
geometry commands. `queue_geometry_command_packed_multi` (82,662 instructions
a frame, 1.27 %) **copies command bytes into a command log and parameter
words into a parameter log** — two flat buffers with write pointers, using an
unrolled 16-byte copy for parameter runs — and returns. Matrix multiplies,
lighting, clipping and vertex transforms happen later, in one pass:

```
update_frame_geometry:                ; from update_frame, at VBlank
    process_geometry(g)               ; → process_geometry_commands: replay the whole log
    geometry_transform_vertexes(g)    ; batch transform (geometry_transform_vertex_block_asm)
    geometry_flush_polygons(g)        ; clip, sort into opaque/translucent lists
    if (swap requested) flip the vertex/polygon double buffer
```

`process_geometry_commands` at 130,867 instructions a frame is the single most
expensive non-rendering function in SM64DS, and it runs **once**. The
per-command dispatch, matrix-stack bookkeeping and state changes are done in a
tight loop over a contiguous buffer with the whole state hot in cache, rather
than 30,000 times through the store helper with the guest's registers
pinned. The vertex transform is then a separate batched stage over an array
of vertices (`geometry_transform_vertex_block_asm`, 38,281 instructions),
which is what lets it be a NEON kernel at all.

**Observable state is computed on demand.** The guest *can* see the geometry
engine mid-frame: `GXSTAT` reports FIFO fullness, the busy flag and the
matrix-stack level; `POLYGON_COUNT` / `VERTEX_COUNT` report totals; and the
matrix read-back registers return the current matrix. `geometry_load_gxstat`
handles the first case by **replaying the log up to now** before answering —
`process_geometry_commands` on whatever is queued, then compacting the
unconsumed tail to the start of the buffers. Games poll `GXSTAT` waiting for
a swap or for FIFO space, so this is not rare (730 instructions a frame in
SM64DS, and pathological in some titles — see the project's notes on
Dragon Ball's 25 k polls a frame). The replay-on-read is what keeps the
deferral *correct*; the FIFO-half-empty IRQ and DMA are synthesised from the
log's fill level the same way.

The 3D render itself is kicked at line 215 (`update_frame_3d` from the
scanline event at `0xd7`), 23 lines before the next frame's VBlank, and joins
the workers described in [02 §1](02-3d-software-rasteriser.md); on the
threaded path `video_3d_run_thread` runs the bin loop on its own thread and
`update_frame` waits at `video_3d_finish_rendering`.

---

## 7. Frame pacing

Two mechanisms, layered:

- **Audio buffer occupancy** (`audio_sync`, [05 §7](05-spu-and-audio-output.md))
  blocks the emulation thread at VBlank while the output ring is more than
  three-quarters full. With audio enabled this is what holds the emulator at
  real-time speed.
- **`system_frame_sync`** at line 215 compares a virtual-frame tick counter
  (advanced by the configured frame duration) against `get_ticks_us`, sleeps
  for the difference, and drives the frame-skip state machine: fall more than
  9,000 µs behind and it skips the next frame's rendering (`engine_a/b.disabled`,
  set in `start_frame`, which makes `render_scanline` a no-op through a null
  `screen` pointer); fall more than 200,000 µs behind and it resynchronises
  the clock instead of trying to catch up. Skipped frames still run the CPUs,
  the scheduler, the geometry replay and the SPU — only rendering is dropped.

---

## 8. Threads

| Thread | Work | Sync |
|---|---|---|
| Main | Both CPUs, scheduler, events, DMA, SPU, geometry replay, **2D engine A**, 3D bins share | — |
| `video_render_thread` | **2D engine B**, all 192 lines | mutex + condvar pair, one frame at a time |
| `video_3d_run_thread` (threaded mode) | `update_frame_3d_*`: bin, then dispatch bins | mutex + condvar, joined in `update_frame` |
| `video_3d_render_thread` × N | `video_3d_render_bins_*`: 12 ÷ (N+1) bins each | mutex + condvar per worker |

All hand-offs are `pthread_mutex` + `pthread_cond`; nothing spins. Sample
counts from the benchmark run — 5,518 / 1,870 / 1,510 / 1,318 — show the
main thread carrying roughly half the machine and the three others sharing
the rest, which on a four-core A55 is about as even as a design with one
serial CPU thread can get.

---

## 9. Summary

| Technique | Effect |
|---|---|
| Delta-encoded fixed-slot event list; slice = head delta | Event dispatch is a compare and a subtract; no absolute-time comparisons |
| Both CPUs run the same slice, ARM7 at doubled cycle cost | Two CPUs, one clock, no per-instruction sync |
| Forced task switch at the next 128-cycle boundary | Fine interleaving only when a cross-CPU dependency exists |
| Timers derived from `cycles - start` on read; overflow as events | Zero cost for running timers |
| `irq_pending = -IME & IF & IE` computed at the source | Translated code never checks interrupts |
| `pending_actions` + `cpu_alerts_*` thunks | Interrupts and code invalidation handled at block boundaries only |
| Whole-transfer DMA over a 16-entry region table | One resolve per region span, no page-table walk |
| DMA cycle tables + completion event | Correct-looking timing, copy already done |
| Coarse/fine code bitmaps ORed over the destination | JIT flush only when DMA actually hits translated code |
| 2D rendered in one batch at VBlank, engine B on a worker | Per-scanline cost paid once per frame, two engines in parallel |
| 32 K-entry per-engine journal of mid-frame writes, replayed per line | Raster effects exact without per-line rendering |
| Copy-on-first-write shadow palette/OAM with silent-store filter | Renderer sees frame-start state; guest sees its own writes |
| VRAM remaps deferred to render time | Bank toggling costs one remap |
| Geometry commands logged, replayed once at VBlank | Command dispatch hot in cache; vertex transform becomes a batch kernel |
| `GXSTAT` read replays the log first | Deferral stays observably correct |
| Audio occupancy as the primary frame limiter | Pacing tied to the audio device, not a timer |
| Frame skip drops rendering only | CPU, geometry and audio stay exact under load |
