# The ARM→AArch64 recompiler

Measured contribution: replacing the interpreter with the recompiler takes
Super Mario 64 DS from 10.64 ms/frame to 2.98 ms/frame on a Cortex-A55 at
2 GHz. Translated code accounts for 14.7 % of all cycles and the runtime that
supports it for another 12 %; translation itself is 0.2 %. See
[00-method-and-measurements.md](00-method-and-measurements.md).

The design goal is visible throughout: **make the common case cost nothing, and
pay for correctness only where the guest actually does something awkward.** There
is no IR, no register allocator, no optimiser, and no profiling. What there is
instead is a very carefully chosen static mapping from guest machine to host
machine, so that most ARM instructions become one AArch64 instruction with no
bookkeeping around them.

---

## 1. The guest machine lives in host registers, permanently

DraStic emulates two CPUs, an ARM946E-S (ARM9) and an ARM7TDMI. Both have
r0–r15 plus CPSR. AArch64 has 31 general-purpose registers. DraStic spends
almost all of them:

| Host register | Holds |
|---|---|
| `w13`–`w27` | **Guest r0–r14** (guest `rN` → host `wN+13`) |
| `w12` | Cycle counter for the current timeslice |
| `x28` | Pointer to the current CPU's state struct |
| `x9` | Base of that CPU's memory page table (see §4) |
| `x10` | Memory-map offset |
| `x11` | Base of the translation cache |
| `x30` | Address of the next block to run |
| `NZCV` | **Guest CPSR condition flags, in the real host flags** |

The mapping `rN → wN+13` is directly readable in the translator: the operand
decoders compute `(instruction >> 16) & 0xf` and add `0xd`, with `0xf` (the PC)
special-cased. The idiom appears 52 times across the translation sources.

Two consequences make most of the recompiler's speed:

**Guest ALU operations become one host instruction.** `ADD r1, r2, r3` is
`add w14, w15, w16`. No load, no store, no shuffling.

**Guest condition codes are host condition codes.** The DS's ARM code is dense
with conditional execution and flag-setting arithmetic. Because guest NZCV *is*
host NZCV, an `ADDS` translates to an `ADDS` and a following `MOVEQ` translates
to a `CSEL`/predicated form — the flags are simply already correct. Emulators
that keep a software flags word pay a few instructions on every flag-setting
operation and a few more on every use; here both are free.

### The AAPCS64 trick

The mapping is not arbitrary. AArch64's procedure call standard makes `x19`–`x28`
callee-saved and `x0`–`x18` caller-saved. DraStic put guest **r6–r14 in w19–w27**,
the callee-saved half.

So when translated code calls a C helper — a memory helper, a block lookup, an
I/O handler — nine of the fifteen guest registers survive the call *because the
C compiler already saves them*. The helper only has to preserve the caller-saved
remainder. That is exactly what the helpers do; the prologue is always the same
seven instructions:

```
stp  w12, w13, [x9, #-224]     ; cycle counter + guest r0
stp  w14, w15, [x9, #-216]     ; guest r1, r2
stp  w16, w17, [x9, #-208]     ; guest r3, r4
str  w18, [x28, #8968]         ; guest r5
mrs  x2, nzcv                  ; guest flags
str  w2,  [x28, #9044]
```

and the epilogue reloads those plus `x9`/`x10`/`x11` and `msr nzcv`. Six guest
registers spilled instead of fifteen, on every helper call, for free, by
choosing which host registers to use.

Note also that the spill slots are addressed at *negative* offsets from `x9`.
`x9` normally holds the page-table base, which sits at offset `0x23d0` inside the
CPU struct — so the same register serves as both the page-table base for memory
access and the spill area base for helper calls. One register, two jobs.

---

## 2. Blocks: a two-instruction timeslice check and lazy cycle accounting

A translated block is laid out as:

```
  [-4] header word: byte offset of this block's metadata record
  [ 0] TBZ  w12, #31, +8      ; cycle budget not exhausted? skip the next insn
  [ 4] BL   recompiler_cpu_next_action_...   ; budget exhausted: switch CPUs
  [ 8] ...first translated instruction...
```

The entry point a branch jumps to is `[0]`; the entry point used when the
scheduler has *just* run events is `[8]`, because the check would be redundant.
`cpu_block_lookup_base` returns `[0]` and every caller that has already
serviced events adds 8. So the per-block cost of timeslice management is
**one not-taken conditional branch**, and zero when re-entering after an event.

### Cycles are accumulated, not counted

The obvious way to charge cycles is to emit a subtract per guest instruction.
DraStic instead accumulates the cost in the translator and emits a single

```
sub w12, w12, #total
```

only when it reaches an instruction flagged as needing exact timing, or at the
end of the block. A straight run of thirty guest instructions costs one
subtract, not thirty. If the total exceeds 4095 it emits a second subtract with
`lsl #12` to cover the high bits.

Cycle cost is computed differently per CPU — the ARM7 path doubles the
instruction's code cycles, the ARM9 path adds a system-wide adjustment word —
which is where DraStic's per-game timing hacks
(`apply_cycle_adjustment_hacks`) get their leverage.

The counter runs **downward and is tested by sign**: `execute_events` adds the
slice length to `w12`, and the block prologue's `TBZ w12, #31` exits when it goes
negative. Testing the sign bit is free; comparing against a limit would not be.

---

## 3. Dispatch: three mechanisms, in order of cheapness

### 3a. Direct linking (the common case)

When a block ends in a branch to a statically known target, the translator emits
a plain `B` to that block's entry point (`cpu_translate_direct_link`). If the
target has not been translated yet, the branch's address is recorded on a patch
list and `cpu_translate_link_delayed_targets` fills it in later. Cost of a
taken guest branch: **one host branch.**

The link chooses between the target's `[0]` and `[8]` entry points — that is,
it can *elide the callee's timeslice check* when the flags on the branch record
say it is safe to do so. There is also a path that emits `mov w12, #-1` before
the branch, forcing the cycle counter negative so the next block immediately
exits to the scheduler; this is how a guest spin-loop is prevented from burning
a whole timeslice spinning. (The exact predicate for that case is not fully
recovered; the emitted code is unambiguous.)

### 3b. The inline branch-target cache (indirect branches)

`BX`, `MOV pc, ...`, and returns cannot be linked statically. DraStic handles
them with a **1024-entry direct-mapped cache embedded in the CPU state struct** —
a tag array at `x28+0x80` and a value array at `x28+0x1080`, 4 KB each:

```
block_indirect_branch:
    and  w1, w0, #0xffc          ; index = (pc >> 2) & 0x3ff, pre-scaled
    add  w2, w1, #0x80           ; tag array
    add  w1, w2, #0x1000         ; value array
    ldr  w2, [x28, w2, uxtw]     ; tag = full guest PC
    ldr  w1, [x28, w1, uxtw]     ; value = 32-bit translation-cache offset
    sub  w2, w2, w0
    cbnz w2, miss
    add  x1, x11, w1, uxtw       ; cache base + offset
    br   x1
```

**Nine instructions, two loads, no call, no hash** for an indirect branch that
hits. Only a miss falls through to `cpu_block_lookup_base`, and that is the
1.24 % `perf` attributes to it. `block_indirect_branch` itself is the single
largest JIT-runtime symbol at 1.56–1.63 % of cycles, which is the price of every
`BX`/return in the entire guest system.

Note the values are **32-bit offsets from the translation-cache base**, not
64-bit pointers. This halves the table, and is used consistently: block
pointers, metadata references and patch targets are all cache-relative 32-bit
quantities. On a machine where the whole code cache is far smaller than 4 GB
this is free, and it doubles the number of entries per cache line.

### 3c. Direct-mapped tables for ITCM

The ARM9's 32 KB instruction tightly-coupled memory is where games put their
hottest code, and it gets rewritten. It gets its own table, indexed directly by
PC with no tag at all — because a table with one entry per possible ITCM
instruction can be exact:

```
block_itcm_branch_arm:              block_itcm_branch_thumb:
    ldr  x1, [x28, #8816]               ldr  x1, [x28, #8824]
    ubfx w2, w0, #2, #13    ; 8192      ubfx w2, w0, #1, #14   ; 16384
    ldr  w1, [x1, w2, uxtw #2]          ldr  w1, [x1, w2, uxtw #2]
    cbz  w1, miss                       cbz  w1, miss
    add  x1, x11, w1, uxtw              add  x1, x11, w1, uxtw
    br   x1                             br   x1
```

32 KB / 4 = 8192 ARM slots, 32 KB / 2 = 16384 Thumb slots. **Six instructions,
no tag compare, no possibility of a false hit.** Zero means "not translated".

---

## 4. Memory: a 16 MB flat page table per CPU

Guest memory access is the other half of any ARM emulator's cost. DraStic uses a
**flat, direct-mapped software page table with 2 KB pages**, one per CPU, giving
2²¹ entries × 8 bytes = **16 MB per CPU**. (The two CPU structs are ~16.08 MB
apart in the system struct, which is that table plus ~9 KB of actual CPU state.)

Spending 32 MB of a device's RAM on page tables looks profligate. It buys this:

```
arm64_load_memory8_unsigned:
    orr  w1, wzr, w0, lsr #11     ; page number
    ldr  x1, [x9, w1, uxtw #3]    ; page table entry
    orr  x1, xzr, x1, lsl #2      ; entry << 2  →  host base
    cbz  x1, slow_path
    ldrb w0, [x1, w0, uxtw]       ; host base + FULL guest address
    ret
```

**Five instructions, one dependent load, no tag compare, no masking.** Three
encoding tricks are stacked here:

- **The base is pre-biased.** The entry stores `(host_page_base − guest_page_base)`,
  so the final load indexes with the *whole* guest address and never has to mask
  off the page offset.
- **The entry is stored shifted right by 2** and recovered with `lsl #2`. This
  frees the top two bits for flags while keeping the arithmetic to one shift that
  the load can fold anyway.
- **Zero means "not directly addressable".** Unmapped memory, I/O registers, and
  anything with side effects all store a zero base, so the single `cbz` covers
  every special case at once. There is no separate "is this I/O?" test.

Alignment is handled without branching where possible: 16-bit accesses just
`and w0, w0, #0xfffffffe`; 32-bit accesses test `w0 & 3` and divert misaligned
ones to the rotate path the ARM architecture requires.

### The store path, and the two flag bits

Stores need more, because stores can invalidate translated code and can hit I/O.
The two reserved bits carry exactly that:

```
arm64_store_memory32_arm9:
    orr  w2, wzr, w0, lsr #11
    and  w0, w0, #0xfffffffc
    ldr  x2, [x9, w2, uxtw #3]
    orr  x3, xzr, x2, lsl #2
    tbnz x2, #62, special          ; bit 62: not a plain writable page
    str  w1, [x3, w0, uxtw]        ; fast path: five instructions
    ret
```

- **bit 62** — this page needs the slow path (I/O, unmapped, or code).
- **bit 63** — the reason is that translated code lives here; do the SMC check.

Loads never test either bit, because loads never care: a code page still has a
valid base, so `cbz` passes and the read goes straight through.

---

## 5. Self-modifying code, filtered three times

DS games rewrite code constantly — overlays are DMA'd into ITCM and main RAM as
a matter of course. Invalidating translations naively would be ruinous. DraStic
filters in three stages, cheapest first:

**Stage 1 — is this even a code page?** `tbnz x2, #63`. One bit test.

**Stage 2 — silent-store elimination.** Before invalidating anything, compare the
value being written against what is already there:

```
    ldr  w2, [x3, w0, uxtw]      ; old value
    sub  w4, w1, w2
    cbnz w4, changed
    ret                          ; identical → nothing happens at all
```

A game that re-uploads the same overlay, or memsets a region that already holds
those bytes, or writes back an unchanged word, pays **one extra load and compare**
and no invalidation. This is the filter that makes the whole scheme viable.

**Stage 3 — does a translated block actually cover this address?**
`cpu_block_check_allocation32` consults an allocation structure and returns
whether anything needs flushing. Only then does the expensive path run.

### Bailing out of a block that just invalidated itself

The hard case is a store that invalidates the block currently executing. DraStic
handles it by recovering the current guest PC from the block's own metadata,
flushing, re-looking-up, and jumping to the new translation:

```
    bl   cpu_block_check_allocation32
    cbz  w0, no_flush_needed
    ldr  x0, [x28, #9064]        ; saved return address into the running block
    ldr  x1, [x28, #9056]        ; translation cache base
    bl   cpu_translate_get_pc    ; → the guest PC that host address corresponds to
    str  w0, [x28, #9148]
    bl   cpu_block_flush
    bl   cpu_block_lookup_base
    br   x0                      ; resume in the fresh translation
```

`cpu_translate_get_pc` is possible because every block records a compressed
host-offset → guest-PC map (`cpu_translate_store_pc_metadata` writes pairs of
16-bit deltas, one per instruction that needs to be locatable). This is the same
machinery that makes exceptions and interrupts land on the right guest
instruction.

---

## 6. The code cache: three arenas, allocated from both ends

`cpu_translate_block` picks one of **three independent arenas** by guest address:

| Arena | Covers |
|---|---|
| ITCM | ARM9 addresses below `0x02000000` |
| Main RAM | addresses with `pc >> 24 == 2` |
| Other | everything else (BIOS, WRAM, VRAM-mapped code) |

with a matching set of flush entry points (`translation_cache_flush_itcm`,
`_main`, `_alternate`). ITCM is invalidated far more often than anything else;
keeping it in its own arena means flushing it does not throw away every
translation of main RAM.

Within an arena, **code grows forward and metadata grows backward** from opposite
ends of the same region. Each arena tracks a code pointer and a metadata pointer;
`cpu_translate_block` writes its 24-byte metadata record at
`metadata_ptr - 0x18` and moves both inward. One allocation region, two
allocators, no fragmentation between them, and the metadata record for a block
is reachable from the block's own header word as a single 32-bit offset.

The metadata record holds, among other things, the block's guest PC at `+4` —
which is what the interrupt path reads (`ldr w1, [x0, #4]`) to discover where the
guest was when the interrupt arrived.

---

## 7. Instruction translation: direct encoding, and a little constant folding

There is no IR. `cpu_translate_instruction` and its helpers write finished
32-bit AArch64 words straight into the arena, with the opcode as a hex constant
ORed with the operand fields. Reading the emitters is reading the encoding
manual:

- `0x36f8000c | off<<5` — `TBZ w12, #31, off` (the timeslice check)
- `0x5100018c | imm<<10` — `SUB w12, w12, #imm` (cycle accounting)
- `0x94000000 | off` — `BL` to a helper
- `0x14000000 | off` — `B` to a linked block
- `0xd51b4200` — `MSR NZCV, x0`
- `0x52800000` / `0x72a00000` / `0x12800000` — `MOVZ` / `MOVK` / `MOVN`

Translation cost is therefore roughly "one store per emitted instruction", which
is why `cpu_block_create` is 0.20 % of cycles and translation never appears as a
stall.

Two pieces of real analysis do happen:

**Constant tracking.** Operand emitters check a per-register "known value" slot
and, when the value is known, materialise it with `MOVZ`/`MOVN`/`MOVK` — or with a
single `ORR` against a logical immediate when the constant happens to be
encodable as one (`cpu_translate_imm_map_to_logical`) — instead of reading the
guest register. Constant addresses and constant operands, which ARM code
generates constantly via literal pools and `MOV`/`ORR` pairs, collapse.

**Intra-block branch patching.** Conditional guest instructions inside a block
become forward branches whose targets are unknown until the block is finished;
a fix-up list at the end of `cpu_translate_block` patches all of them in one
pass.

Special cases get their own emitters rather than generic handling —
`cpu_translate_msr_op`, `cpu_translate_mrs_op`, `cpu_translate_bx_op`,
`cpu_translate_raise_exception`, `cpu_translate_block_memory_op` (LDM/STM),
`cpu_translate_conditional_skip`. LDM/STM in particular get sixteen specialised
helpers each (`arm64_load_block1` … `arm64_load_block16`,
`arm64_store_block1` … `_16`), one per register-count, so a multi-register
transfer is a call to a straight-line routine rather than a loop.

---

## 8. The I/O fast path that belongs to the 3D engine

One detail sits at the boundary between this document and
[the 3D one](02-3d-software-rasteriser.md). The ARM9 32-bit store helper does not
route the geometry command ports through the generic I/O dispatcher. It
recognises them inline:

```
    orr  w4, wzr, w0, lsr #24
    sub  w4, w4, #0x4
    cbnz w4, not_io                 ; not 0x04xxxxxx
    ubfx w1, w0, #0, #24
    sub  w3, w1, #0x400
    cmp  w3, #0x1fc
    b.hi generic_io_store           ; outside 0x4000400..0x40005fc
    cmp  w3, #0x40
    b.ge queue_geometry_command     ; 0x4000440+ : direct command ports
    b    queue_geometry_command_packed  ; 0x4000400 : GXFIFO
```

3D games push their entire display list through these addresses, one 32-bit store
per command word. Giving `GXFIFO` its own branch out of the store helper removes
the generic I/O dispatch from the hottest store in any 3D game.
`queue_geometry_command_packed_multi` alone accounts for 1.27 % of all guest-
visible instructions executed in SM64DS (82,662 per frame) — reaching it through
the general `store_io_register_arm9_32` path would add the dispatch cost to every
one of those.

---

## 9. Summary

The recompiler is fast because of a set of decisions that each remove a fixed
per-operation cost rather than because of any clever optimisation:

| Decision | What it removes |
|---|---|
| Guest r0–r14 pinned to `w13`–`w27` | Load/store around every guest ALU op |
| r6–r14 in the AAPCS64 callee-saved range | Nine of fifteen register spills per helper call |
| Guest CPSR flags in host NZCV | Software flag computation and testing |
| Accumulated cycle counts, sign-bit test | Per-instruction cycle bookkeeping |
| 2-instruction block prologue, elidable | Timeslice checking |
| Direct block linking | Dispatch on static branches |
| 1024-entry inline branch cache | Hash lookup on indirect branches |
| Tag-free ITCM tables | Tag compares in the hottest code region |
| 32-bit cache-relative pointers | Half the footprint of every dispatch table |
| 16 MB flat page table, biased, flag-tagged | Address masking, range checks, I/O tests |
| Silent-store elimination | Almost all SMC invalidation |
| Three arenas | Cross-invalidation between ITCM and main RAM |
| Direct opcode emission | Compilation time |

The recurring shape is that a **larger static table** replaces a **conditional
test**. That is a good trade on any machine and an excellent one on an in-order
Cortex-A55, where a mispredicted branch costs far more than the extra cache
pressure of a big table with predictable access.
