# ARM → AArch64 recompiler

Builds on AArch64 hosts only (`DSPERATE_JIT`); the x86 build runs the
interpreter. The techniques being reimplemented, and the measurements behind
them, are described in `docs/techniques/01-arm-to-aarch64-jit.md`.

**Every line here is authored from that description of behaviour.** Nothing is
transcribed from `objdump`/Ghidra output of any proprietary binary. melonDS's
`ARMJIT_A64` (GPLv3) may be consulted and, with attribution, reused; so far
nothing has been.

## Files

The runtime is host-agnostic; each backend directory provides the encoder,
the stubs and the translator behind the `backend` interface in
`jit_internal.h` (`emit_stubs`, `translate_block`, `write_entry_redirect`,
`patch_link`). CMake picks the directory by host.

| file | role |
|---|---|
| `runtime.cpp` | code arena, block cache, LUT, block linking, park-and-revive, SMC tracking by host page, the helpers translated code calls |
| `jit.h` | public API: `attach`, `run` (a `RunFn`), `flush`, `set_trace`, `stats` |
| `jit_internal.h` | context offsets, block/runtime structures, the backend interface |
| `block_shape.h` | where a block ends -- shared so every backend cuts blocks identically and their frame hashes are comparable |
| `a64/emit.h` | AArch64 encoder: only the forms the translator and stubs use. Verified against `aarch64-linux-gnu-objdump`. |
| `a64/convention.h` | the AArch64 register map (guest registers pinned in x9-x13, x19-x28) |
| `a64/stubs.cpp` | the AArch64 stubs and code patches |
| `a64/translate.cpp` | one-pass block translator for ARM and Thumb, flag-liveness pre-pass, cycle accounting |
| `a32/emit.h` | A32 (ARMv7) encoder, ARM state |
| `a32/convention.h` | the ARMv7 register map: guest r0-r3 and sp pinned in r4-r8, budget r9, page table r10, context r11; the other guest registers in memory and a per-block cache; guest NZCVQ in the APSR |
| `a32/stubs.cpp` | the ARMv7 stubs (AAPCS32: `blx` to Thumb-2 helpers, struct return in memory, 8-byte stack frames) |
| `a32/translate.cpp` | ARMv7 translator: register cache, data processing / multiplies / branches (static and indirect) by field substitution with native flags and predication, single loads/stores and same-page LDM/STM (pc included) with a page-table fast path, same-mode MSR; the rest through the fallback stub; same block shape as a64 |

The ARMv7 backend's gates, run after every step: `test_jit` under qemu-arm (1600 trials), the five replay
scenes' 300-frame hashes against the AArch64 JIT's, and strict mode against
the interpreter. Phase 2 step 2 (2026-09-03, memory inline): all five
scenes identical to the AArch64 JIT (etody included, once memory stopped
going through the polling fallback stub), strict mode identical to the
interpreter. The page-table entry is pointer-sized (`mem::Entry` is
`uintptr_t`, tags in the top two bits), so the 32-bit probe is one `ldr`.

Cold-path rule for the register cache: a cold path is emitted in the cache
state of its branch point, writes that state's dirty slots back, does its
work, and reconciles to the hot path's end state by reloading every slot
that state holds -- so the hot path may evict freely after the branch. For
LDM/STM the cold path is the interpreter, so the base writeback waits in a
temporary until the transfer is done, and a conditional memory body keeps
the flags across its page tests even when nothing after it reads them: the
interpreter re-evaluates the condition on them.

Phase 2 step 3 (2026-09-03): the indirect-branch stubs (`branch_indirect`,
`branch_indirect_cdi`) park the guest flags on the stack so the refill and
post-jump CDI arithmetic can compare and predicate freely; BX/BLX, Thumb
hi-register pc forms, unpaired BL suffixes, LDM/POP pc and same-mode MSR
are inline. The directed cases in `tests/jit_test.cpp` cover every one of
those forms on both CPUs, because the fuzzer's generator reaches none of
them; they caught a register-permutation bug the scenes only showed as a
late hash divergence.

Two A32 rules the AArch64 backend never needed: a predicated cycle charge
must come *before* a flag-writing (S) body, because the body rewrites the
flags the predication reads; and anything that must run after such a body
under the same condition (the ARM7 multiply's C clear) uses a branch around
the body instead of predication.

## How it works

**Pinned registers.** Guest r0–r7, r13, r14 live in x19–x28 (callee-saved),
r8–r12 in x9–x13 (spilled by the call stubs), the cycle budget in w8, the
page-table base in x14, the per-page timing table in x15, the code-arena base
in x18, `CpuContext*` in x29. Guest NZCV live in the host NZCV; the rest of
CPSR stays in memory. Because every block agrees on this, linked blocks
reconcile nothing.

**Blocks** are straight-line runs ending at the first branch (or 64
instructions), laid out as a *hot* section (the fast paths, in guest order)
followed by a *cold* section (everything reached only on a rare condition:
budget exhausted, page-table miss, block-transfer fallback). The cold code is
written to a side buffer during translation and spliced in after the hot
code; cross-section branches and cold `bl`s to the stubs are fixups resolved
at the splice. The hot section runs 9–10 host words per guest instruction
(SM64DS: 36.8 bytes; the cold third is never fetched).

**Stubs take literal arguments.** A block reaches a stub with `bl`; the stub
reads its operands from the words after the `bl` through x30 and skips them:
`bl fallback; .word instr; .word key` (3 words, was ~30 inline),
`bl poll; .word next_key`, `bl link; .word key`. A linked `bl link` becomes a
bare `b` whose literal is never executed, so a linked branch costs one
instruction. Indirect branches go through a per-CPU stub that updates T,
charges the pipeline refill and looks the key up in a direct-mapped LUT
(`(native offset << 32) | key`, key = pc | T), falling back to a hash map and
then to translation. The LUTs sit at the front of the code arena and the arena
base is pinned in x18, so the probe materialises no constants and the same
register serves both the probe and the jump to the block: **seven instructions
on the ARM9, eight on the ARM7** (`ubfx`, `ldr`, `eor`, `cbnz`, `lsr`, `add`,
`br`). See "The branch LUT" below for the sizing. The budget register holds
`budget - 1`, so every budget test is one `tbnz w8, #31` (the stubs add and
subtract the one at the C boundary).

**Memory.** Loads and stores inline the tagged page-table fast path
(`lsr, ldr, lsl, cbz, ldr`); stores also test the two tag bits, so MMIO,
read-only and *code* pages leave the fast path. The timing-table load and the
address arithmetic are issued under the page-entry load, and the charge under
the data load, so the in-order A55 stalls on neither. The slow path (cold)
calls `jit_h_ld*/st*` — the interpreter's own `mem_read*/mem_write*` paths
minus the cost — through a pure-call stub that spills only the caller-saved
guest registers, then repeats the tail (writeback, cost) and polls; the
whole instruction is never re-executed by the interpreter. LDM/STM/PUSH/POP
take the fast path when the whole transfer sits in one 2 KB page and fall
back to the interpreter otherwise.

**Fallback.** Anything not inlined — MSR that changes mode, SWI, coprocessor
reads, PC-destination ALU ops, user-bank transfers, SWP, LDRD/STRD, the v5
DSP extensions, undefined encodings — calls `jit_h_fallback`, which executes
that one instruction through the interpreter with the interpreter's own
cycle accounting, then polls budget / alerts / IRQ and returns to the block
or dispatches on the new pc. The JIT is therefore complete and exact from
the first build; inlining is an optimisation, and every inlined form is
checked against the interpreter by `tests/jit_test.cpp`. Inlined since the
first build: CP15 cache-maintenance `MCR`s the core ignores (`numC + 2`),
and `MSR CPSR_<fields>` when the mode does not change (tested at run time;
I/F go to the memory copy, the flags field to the host NZCV, and the block
polls for an unmasked IRQ).

**Cycles.** The interpreter's model (`cpu_cycles.h`) is reproduced exactly:
code-fetch cost is static per instruction (the TCM windows and PU cache map
are baked into the per-page table, so it is one lookup for both engines),
data cost is read from the same table at run time, and the CD/CDI combine
formulas — including the ARM7 main-RAM rules — are emitted inline without
touching flags. On the ARM9 `max(nc + nd - 6, nc, nd)` with `nd >= 1`
collapses to `nd` for `nc <= 1` (one `sub`), `max(nc, nd)` for `nc <= 6` and
`nc + max(nd - 6, 0)` above. Static costs are batched into one `sub` per run;
a conditional instruction whose cost is only its fetch cost charges it before
the condition test, so the skipped path needs no code. In `DS_JIT_STRICT=1`
mode the budget is also tested after every instruction, so the two engines
interleave identically and frame dumps must be byte-equal; normally it is
tested once per block.

**Fast timing** (`emu.timing = fast`, the default; `core/cpu/timing_mode.h`).
The exact model above costs several instructions per memory access (the
timing-table load and the CD/CDI combine; on the ARM7 the main-RAM rule is
~12) and ~20 per indirect branch (the refill lookup). The fast model charges
every data access a constant and every jump a constant refill, loads and
stores pay `numC + data` with no overlap, so an access costs one `sub` and the
refill one `movz`. numC stays exact: it is resolved at translation time anyway.
The interpreter implements the same model (`cpu_cycles.h`), so strict mode
still makes the two engines byte-equal; `test_jit` runs every trial under both
models, and the stubs are re-emitted when the model changes between attaches.
The ARM9's data constant depends on the base register, known at translation:
SP-based accesses (the stack, in DTCM) cost 0, all others 5; the ARM7's cost
2; refills 4 / 2 (`DS_FAST=d9s,d9,d7,r9,r7` overrides them). The interpreter
derives the same split from the opcode (`fast_sp_based`). One constant cannot
fit: exact overlaps a DTCM access with the fetch, so stack-heavy frame logic
(ST, NSMB) pays nothing for data, while main-RAM-bound idle work (PW2's) pays
~5. A flat 4 pushed ST's and NSMB's frame logic past VBlank and halved their 3D
rate while audio kept pace (the host time it "saved" was the work the game no
longer did); a flat 2 fixed that but made PW2 iterate ~40% more per frame.
Check a change against 3D swap counts under exact (`DS_PROFILE`,
"gx swap_buffers"), not only host time: ST sits at its deadline even under
exact, and (1, 5) already drops it from 2255 swaps to 2053.

**Flags.** A backward liveness pass per block records which of N, Z, C, V are
live after each instruction. Logical ops then use a bare `tst` when C/V are
dead; when they are not, a shared stub merges (`bl merge_keep_cv` /
`merge_set_c`, two or three words at the site). Arithmetic maps to
`adds/subs/adcs/sbcs` directly (same carry convention).

**Self-modifying code.** Code pages are tracked by *host* page so that the
other CPU's mapping and DMA see the same `TAG_CODE` bit (which requires
every host backing buffer to be `PAGE_SIZE`-aligned -- `alloc_page_buf` --
so a host page is exactly one guest page; `PageTable::map` asserts it); remaps re-apply it
through `PageTable::code_query`, and `set_code_host` finds the guest pages of
a host page through a reverse index rebuilt lazily after a remap. Every store
path that lands on a tagged page goes through `mem::store_code`, which filters
twice before anything is invalidated (DraStic's second and third SMC stages):
a value identical to what is already there is not a modification and is not
reported at all, and a changed store kills only the blocks whose guest bytes
it overlaps (`invalidate_host_range`; a `Block` records its first and last
host byte), not every block on the 2 KB page. Both matter: on Golden Sun Dark
Dawn ITCM data shares a page with the hot ITCM routines, and before the
filters each data store there killed ~26 blocks -- 1.9k retranslations a
frame, 7 % of the frame; after, ~60. A killed block has its entry patched to
a redirect into the dispatcher and its LUT/map entries removed, and the alert
word is raised so a block currently executing leaves at its next poll.
`DS_JIT_CHURN=1` reports the writers, the pages and the retranslated blocks.

**Interrupts** are taken on the C side: translated code leaves when a poll
finds `irq_pending` set with I clear, `run()` calls `check_irq()`, and
re-enters at the new pc. Between slices the scheduler's events set
`irq_pending`, and `run()` checks it first, as the interpreter does. A halt
inside a helper raises `ALERT_HALTED`; `run()` records `budget_at_halt` and
ends the slice, exactly as the interpreter's loop does.

## Verification

1. `tests/jit_test.cpp` (cross-built, runs under qemu): random straight-line
   ARM and Thumb sequences on both CPUs, run by the interpreter and the JIT
   from identical state; registers, flags, consumed cycles and memory must
   match. Failures are shrunk to the shortest failing prefix. Directed cases
   cover what the generator reaches rarely (pending cycles across a slow
   access, CP15 no-ops, MSR with and without a mode change). Every trial ends
   in a halt, and both engines end a slice with budget −1, so the consumed
   cycles are compared through `CpuContext::budget_at_halt` — until
   2026-08-21 the budget comparison was −1 against −1 and saw nothing; the
   lost-pending-cycles bug below dates from then. Bugs it has caught so far:
   CLZ inlined on the ARM7, unaligned Thumb LDM bases, the ARM7 multiply
   clearing C, register shifts by 64/96/128 taking the "exactly 32" carry
   rule, a fallback instruction observing flags the liveness pass had marked
   dead, and the static cycles pending before a load being charged only on
   its fast path.
2. `dsperate --interp` vs default on the same ROM with `--dump-frames`
   (`DS_JIT_STRICT=1` for byte equality) and `--trace` (the JIT calls the same
   trace hook per instruction when tracing is on).
3. Against melonDS exactly as the interpreter is.

## Debugging

- `DS_JIT_STRICT=1`: per-instruction budget checks (lockstep with the interpreter).
- `DS_JIT_DEBUG=1`: log every fallback with pc, instruction and resulting state; dump every translated block as hex words.
- `DS_JIT_HIST=1` (with `DS_PROFILE=1`): fallback counters and the hottest fallback sites at exit.
- `DS_DEBUG_TIMING=1`: log every ARM9 timing-table rebuild (each one drops every translated block).
- `--interp`: run both CPUs on the interpreter.

## Measured on the RK3566 (Cortex-A55 @ 1.99 GHz)

Super Mario 64 DS, 300 frames from direct boot, `DS_PROFILE=1` (CPU stage
times; `perf` works on the device: `perf stat`/`perf record` with the A55
PMU, JIT code shows as `[JIT]`):

| build | ARM9 | ARM7 | frame (profiled sum) |
|---|---|---|---|
| interpreter | 9.44 s | 1.16 s | 51 ms |
| JIT, first build (2026-08-21 a.m.) | 2.91 s | 1.11 s | 28.9 ms |
| + cold sections, literal-argument stubs, slow-path helpers, ARM9 charge collapse | 2.46 s | 0.99 s | 27.1 ms |
| + MSR/MCR inline, no-op CP15 writes skipped, `set_code_host` index, LUT-first entry | 1.75 s | 0.71 s | 23.8 ms |
| + cached next deadline, DMA running mask | 1.72 s | 0.70 s | 23.7 ms (25.4 wall) |
| + 2D/3D renderer passes (not the JIT), range-limited timing rebuilds | 1.52 s | 0.69 s | 15.1 ms |
| + native slice loop (2026-08-22) | 1.55 s | 0.65 s | 14.9 ms wall (MPH 19.1, M&L 10.4, Spectrobes 8.6) |

The game executes ~292 k ARM9 instructions per frame; translated code is
**13.7 % of the run** (≈3.3 ms/frame for both CPUs, ≈18 host cycles per
guest instruction). The I-cache theory of the first build was wrong: `perf
stat` shows 18.8 M L1I misses in 300 frames (~3 % of cycles). What the
profile actually showed, and what was done:

- `Timing::update_cpu9` (7 %) and `PageTable::set_code_host` (6 %): every
  CP15 TCM/PU write rebuilt the 1 M-entry timing table and dropped every
  block, even when the value was unchanged; every code-tag flip scanned
  128 k page entries. Now: unchanged writes are skipped, `update_tcm` only
  remaps when the windows changed, and the tag flip uses a reverse index.
  What remains of `update_cpu9` is boot.
- Fallbacks: 2.14 M → 0.19 M per 300 frames (memory slow paths are pure
  calls; CP15 cache ops and same-mode MSR are inline).
- Per-slice overhead (scheduler loop, `enter`/`exit`, `run()`): ~14 % of the
  run, ~450 ns per 128-cycle slice, 7.3 k slices per frame.

**Native slice loop** (done 2026-08-22, `Runtime::run_loop`,
`Scheduler::slice_next`): the scheduler's slice sequence runs from a loop
in the arena that saves the callee-saved registers once and enters blocks
through `enter_light` (x29/x30 only); `enter` for C callers wraps it. The
C++ slice logic became a straight-line function with two resume points —
the `switch` state machine tried first was slower than the old loop
(jump-table and indirect-call mispredicts on the A55). Worth 2–6 % of wall
time; `slice_next` is still ~6 % (≈300 cycles per slice) and the guest
register save/restore per entry is the rest. Next on this axis: refilling
the budget in place from the poll stub while the other CPU is halted and
no event is due, so the ARM9 never leaves translated code between slices.

### The branch LUT

**64 K entries, 512 KB per CPU** — deliberately *not* the technique's sizing.
`docs/techniques/01` §3b uses 1024 entries (8 KB) so the table stays inside
L1D, and reimplementing that here was tried on 2026-08-24 and **reverted: it
is 1.6-2.7 % slower on the device.**

Miss counts (SM64DS 300 frames from direct boot, via a temporary counter in
`jit_h_lookup` — the dispatch stub's only miss path) against per-frame mean on
the RK3566 (`bench3.sh <bin> 600 3`, three reps, replayed scenes):

| `LUT_BITS` | size/CPU | misses | sm64 | mlbis |
|---|---|---|---|---|
| 10 | 8 KB | 120,859 | 9.155 (+1.6 %) | 6.271 (+1.6 %) |
| 13 | 64 KB | 17,809 | 9.019 | 6.196 |
| **16** | **512 KB** | **4,421** | **8.983** | **6.156** |
| 17 | 1 MB | — | 8.998 | 6.158 |
| 18 | 2 MB | — | 8.990 | 6.194 |

Three things the footprint argument missed:

- **The table is touched sparsely.** Only entries for live blocks are ever
  read, so the working set is the number of hot blocks — a few thousand lines —
  not the table size. Those lines stay cached; the other 500 KB is never
  fetched. "512 KB does not fit in L2" was the wrong model.
- **Our miss path is far dearer than the technique's.** A miss here is
  `call_pure` (spill, flags, budget) into `jit_h_lookup` and an
  `unordered_map::find` — call it ~100 cycles — where the technique falls into
  a hand-written `cpu_block_lookup_base`. Capacity is worth more to us than
  footprint is, which is the opposite of the trade the technique makes.
- **Translated code is only 6-11 % of the process.** The renderer dominates,
  so anything won on the dispatch path is diluted roughly tenfold in frame
  time. Dispatch has to get *much* cheaper before it shows up at all.

Going above 64 K gains nothing, so the table is not capacity-starved either;
16 bits is the plateau.

Next, in order, each measured on the device with the strict slice diff kept
green:

1. The translated code itself (≈18 cycles per guest instruction): the
   dispatcher for indirect branches (`bx lr`: the LUT probe is down from ~15
   instructions to 7, which measured as noise — the refill-cost arithmetic
   ahead of it is still ~20 instructions with internal branches and is the
   real target), conditional execution without the pending flush and via
   `csel` rather than a branch, link through a second entry point that skips
   the budget test, shift-by-register clamps. Note the dilution above: at
   6-11 % of the process, translated code needs a *large* win to move a frame.
2. ARM7 data-cost path (still the full main-RAM rule inline, ~12
   instructions); inline `MSR SPSR` and `LDR pc` for the ARM7 BIOS IRQ
   handler (MPH's ARM7 spins in BIOS `swi 3` with five fallbacks per loop).

Still open, not performance: W^X dual mapping for Android; inlining the v5
DSP ops, SWP, LDRD/STRD; ARM7 straight-line code crossing a 32 KB region
boundary uses the block's own region for fetch costs (the interpreter keeps
the last jump target's).


## Profiling translated code

`DS_PERF_MAP=1` writes `/tmp/perf-<pid>.map` as blocks are translated, so
`perf` resolves JIT samples to `jit9_<pc>` / `jit7_<pc>` (a `t` suffix marks
Thumb) instead of one anonymous mapping. The arena is reused after a flush,
so an address can appear more than once; perf takes the last entry.
The permanent stubs are named too (`jit_stub_<name>`, per-CPU ones
`jit_stub9_`/`jit_stub7_`: dispatch, link, fallback, branch_indirect…), so
dispatch and call overhead shows up under its own names rather than as bare
arena addresses, and a `perf report` listing can be bucketed by subsystem
(JIT code / JIT stubs / JIT runtime / 3D / 2D / scheduler / memory …).

Measured this way on the RK3566 over 60 s of real gameplay, translated code
is 6-11 % of the process and extremely diffuse — the hottest single block is
0.65 % (Mario & Luigi) and 0.19 % elsewhere. The renderer, not the
recompiler, is where the time goes.
