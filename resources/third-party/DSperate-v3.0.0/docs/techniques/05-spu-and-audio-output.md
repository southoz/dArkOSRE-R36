# The SPU and audio output

The DS sound hardware is sixteen channels of PCM8 / PCM16 / IMA-ADPCM / PSG /
noise, each with its own timer-derived sample rate, volume, pan and loop
mode, mixed into a stereo output at 32.768 kHz, plus two capture units that
write the mix (or a channel) back into ARM7 memory for effects. DraStic
emulates all of it in **plain C — there is not one hand-written assembly
kernel in the SPU** — and it costs **1.1 % of cycles** in Super Mario 64 DS on
the Cortex-A55. See [00-method-and-measurements.md](00-method-and-measurements.md).

That figure is the point of this document. The SPU is cheap not because its
inner loop is clever but because of *when* and *how often* it runs, and
because everything per-channel has been reduced to one fixed-point
multiply-add per output sample. The same "decide at the coarsest correct
granularity" theme as the renderers, applied to audio.

Instruction counts below are from the device-free qemu harness (exact guest
instruction counts, SM64DS benchmark, 4,205 frames):

| Function | Calls | Instructions | Per frame | Share |
|---|---|---|---|---|
| `spu_render_samples` | 4,205 (1/frame) | 652 M | 155,051 | **2.39 %** |
| `spu_adpcm_decode_block` | 2.67 M (634/frame) | 80 M | 19,040 | 0.29 % |
| `spu_render_capture` | 8,410 (2/frame) | 6.2 M | 1,481 | 0.02 % |
| `update_spu` | 4,205 (1/frame) | 1.6 M | 383 | 0.01 % |
| `spu_update_channel_settings` | 21,854 (~5/frame) | 73 k | 17 | — |
| `spu_key_on` | 975 | 7 k | 2 | — |
| `audio_callback` (SDL thread) | 139 k | 1.4 M | — | — |

The SPU is 2.4 % of *instructions* but 1.1 % of *cycles*: it is a
cache-friendly straight-line loop with no branch misprediction, so it runs at
better than average IPC. Meteos, a 2D game with a busier soundtrack, spends
0.97 % of its instructions in `spu_render_samples`.

---

## 1. Mix once per frame, not once per sample

`update_spu` is called from exactly one place in normal operation:
`event_scanline_start_function`, at the VBlank line (192), immediately after
`update_frame`. The call count confirms it: 4,205 calls in 4,205 frames.

```
lines = 192:                              ; VBlank
    update_frame(video)                   ; finish 2D, present
    update_input()
    spu->frame_cycles = system->cycles
    if (!audio_disabled)  update_spu(system)
    if (audio_sync)       audio_sync(spu)  ; throttle — see §7
```

`update_spu` computes how many *output* samples have elapsed since the last
call in fixed point:

```
elapsed  = (system->cycles << 10) - spu->last_cycles_fp        ; 22.10 cycles
samples  = (elapsed * spu->samples_per_cycle) >> 32            ; ≈ 735 at 44.1 kHz
spu->last_cycles_fp += samples * spu->cycles_per_sample        ; carry the remainder
```

`cycles_per_sample` and `samples_per_cycle` are computed once at
initialisation from the ARM9 clock and the *host* output rate (44,100 Hz on
this build: `initialize_spu` stores `0xac44` and prints
"1524.66 cycles per output sample"). Note that the remainder is carried, so a
frame's rounding error is never lost: over time exactly the right number of
samples is produced.

It then zeroes a stack mix buffer of `samples` stereo `int32` pairs, calls
`spu_render_samples` to accumulate every channel into it, runs the two
capture units, clamps to 16 bits and writes into the output ring buffer.

One call, ~735 samples, sixteen channels. Every other emulator design
choice below follows from the decision to run the mixer at frame
granularity: register writes cannot take effect mid-frame, so they are
journaled as dirty bits and resolved at the next mix; channel state is only
loaded and stored once per channel per frame; and the ADPCM decoder can run
ahead in blocks.

**The exception that proves the rule.** `event_timer_overflow_function`
also calls `update_spu` when **ARM7 timer 1** overflows and audio is
enabled. That is a catch-up, not a mixing tick: a game that streams audio
refills its PCM buffer from a timer interrupt, and the mixer has to consume
the old contents *before* the handler overwrites them. Catching up at the
timer the driver uses to pace itself is the cheapest proxy for "the guest is
about to mutate sample memory". In SM64DS this path never fires.

---

## 2. One multiply-add per channel per sample

`spu_render_samples` is two nested loops — channels outside, samples inside —
and the sample loop for the common formats is this small:

```
for each output sample:
    idx    = pos >> 32                     ; 32.32 fixed-point source position
    pos   += step                          ; source samples per output sample
    s      = src[idx]                      ; int16, or int8 << 8 for PCM8
    acc.l += vol_l * s                     ; int32 accumulate
    acc.r += vol_r * s
    if (idx >= end) { loop or stop }       ; one compare, usually not taken
```

Three things are notable:

**Resampling is folded into playback.** There is no separate resampler and
no intermediate 32.768 kHz mix. Each channel's `step` is the ratio of its own
timer-derived rate to the host output rate, computed in
`spu_update_channel_settings` as

```
step = ((CLK/4) / (0x10000 - SOUNDxTMR)) / host_rate      ; 32.32 fixed point
```

so a 16 kHz voice on a 44.1 kHz output simply advances its cursor by 0.36
per output sample. Nearest-neighbour: `src[pos >> 32]`, no interpolation.
That is a deliberate quality trade — the DS's own output is unfiltered
too — and it removes a multiply and a second load from every sample.

**Volume and pan are pre-multiplied into two shorts.** Also in
`spu_update_channel_settings`:

```
v      = (vol == 127 ? 128 : vol) * (master == 127 ? 128 : master) << shift
vol_l  = ((127 - pan) * v) >> 13
vol_r  = (pan * v) >> 13
```

where `shift` is `4 - SOUNDxCNT.volume_div` (0 for the ÷16 setting). Channel
volume, master volume, the volume divider and pan collapse into two
per-channel constants, and the mixer's per-sample work is two `smull`-class
multiplies. The mix accumulates at 12 fractional bits and the final
`>> 12` + clamp happens once, on the whole buffer (§5).

**The source pointer is a host pointer.** `spu_key_on` resolves the guest
`SOUNDxSAD` through the memory region table *once*, at key-on:

```
region = mem->arm7_regions[(sad >> 23) & 0xf]
src    = region.base + (sad & region.address_mask)     ; direct-mapped region
       | region.fn(...)                                ; special region
       | NULL → channel is silent                      ; I/O or unmapped
```

The sample loop indexes host memory directly. There is no page-table lookup,
no `cbz` on a page entry, no per-sample address translation of any kind.

---

## 3. Register writes set dirty bits; the mixer resolves them

`store_io_register_arm7_16` — 8,025 instructions per frame in SM64DS; games
hammer the SPU registers — does not recompute anything for a SOUNDxCNT or
SOUNDxTMR write. It stores the value into the mirrored I/O page and ORs a
bit into the channel's dirty byte:

| Bit | Set by | Recomputed |
|---|---|---|
| 0 | SOUNDxTMR / SOUNDCNT (rate) | `step` |
| 1 | SOUNDxCNT volume, pan, divider / master volume | `vol_l`, `vol_r` |

`spu_render_samples` tests the byte at the top of each channel and calls
`spu_update_channel_settings` only when it is non-zero — 21,854 times in
4,205 frames, about five channel-updates a frame, against tens of thousands of
register writes. The two divides implied by `step` (one 64-bit) are paid five
times a frame instead of per write.

Key-on is the only write handled eagerly, because it has to snapshot the
start address, length, loop point and format before the game changes them
(`spu_key_on`, 975 calls over the whole run). A key-on with zero total length
is ignored outright unless the channel is PSG/noise. The mirrored register
word also keeps the hardware `busy` bit (`SOUNDxCNT.31`), which
`spu_render_samples` clears in place when a one-shot channel ends — so the
guest reads channel status straight from the I/O page with no SPU call at all.

---

## 4. ADPCM is decoded eight samples at a time into a 64-sample ring

IMA-ADPCM is the format nearly every commercial DS game uses for music and
most effects, and it is the only format where the sample loop does anything
beyond a load. `spu_render_samples` keeps the loop body identical to PCM16 by
reading from a **64-entry `int16` ring inside the channel struct** and
refilling it lazily:

```
    if (idx >= chan->decoded_upto)          ; ring has run dry
        do spu_adpcm_decode_block(chan)     ; decode ONE 32-bit word = 8 nibbles
        while (idx >= chan->decoded_upto)
    s = chan->ring[idx & 0x3f]
```

`spu_adpcm_decode_block` is 30 instructions per call for eight samples — under
four instructions per decoded sample — and it is called 634 times a frame
here. It reads one aligned 32-bit word of nibbles, so the source is touched
in 4-byte units rather than per nibble, and writes eight consecutive ring
slots. The decoder itself is the standard IMA step-table recurrence with
`adpcm_step_table` / `adpcm_index_step_table` lookups; the branch-free
`diff = step>>3 (+ step>>2)(+ step>>1)(+ step)` form compiles to conditional
selects.

**Loops do not re-decode from the block start.** The DS defines the loop
point in words, and an ADPCM loop must restart with the predictor state that
was current at that point. The obvious implementation re-decodes from the
start of the sample on every loop. DraStic instead snapshots the predictor
and step index the first time the cursor crosses the loop start
(`chan->loop_predictor = chan->predictor; chan->loop_index = chan->index;
chan->looped = 1`) and, on every subsequent wrap, restores the snapshot and
rewinds `decoded_upto` by the loop length. A looped ADPCM voice costs the
same as a looped PCM16 one.

---

## 5. PSG and noise are table lookups

Channels 8–13 can be PSG square waves and 14–15 noise. DraStic does not
generate either at mix time:

- **PSG.** `psg_samples` is eight 8-entry `int16` tables, one per duty
  cycle. Key-on with format 3 on channels 8–13 points the channel's source
  at `psg_samples + duty * 16` with length 8 and loop length 8 — the channel
  is then an ordinary looping PCM16 voice through the same mixer loop.
- **Noise.** `initialize_spu` runs the DS's 15-bit LFSR
  (`x >>= 1; if (carry) x ^= 0x6000`) for its full period once, at start-up,
  into a 32,767-byte table of `0x7f` / `0x80` (`noise_samples`). Channels
  14–15 with format 3 become a looping PCM8 voice over that table with length
  = loop = `0x7fff`. Noise costs one byte load per sample, like PCM8, and the
  LFSR is never stepped at run time.

Format 3 requested on channels 0–7, which cannot do PSG on hardware, becomes
an internal format 5 that renders silence.

---

## 6. Clamp with NEON, write to a ring, and stub the capture units

After all channels are accumulated the buffer is converted in one pass:
`>> 12`, clamp to `[-32768, 32767]`, narrow to `int16`. Ghidra shows this as
`NEON_smax` / `NEON_smin` over four lanes — **compiler auto-vectorised C**, the
same code appearing three times in `update_spu` (before wrap, after wrap,
no wrap) and once more as `spu_clamp_block`. Eight samples per iteration,
scalar tail. There is no hand-written kernel here because there is nothing
for one to do: the clamp is already bandwidth-bound on a 6 KB buffer.

The output is a **64 K-sample (`0x20000`-byte) ring** at the start of the SPU
struct, with 16-bit read and write indices masked with `& 0xffff`. The
mixer writes from the write index, splitting a write that wraps into two
clamp passes rather than building a contiguous staging buffer. SDL's
`audio_callback` copies from the read index (with an unrolled 16-byte copy
loop) and zero-fills any shortfall. That is the entire host-audio interface:
one producer index, one consumer index, no lock — the callback runs on
SDL's thread and only ever advances the read index.

**Sound capture writes silence.** `spu_render_capture` runs the two capture
units with the right cursor stepping, wrap-or-stop semantics and
`busy`-bit clearing — but the value it stores into the capture buffer is `0`
for every mode. Capture is emulated for its *timing and control* effects,
which games observe, and not for its data, which they rarely do audibly
(capture-based reverb is what is lost). Channels 0–3 do keep their current
sample aside in a four-slot side buffer, which is what `SOUNDCNT`'s
output-selection bits (mixer / channel 1 / channel 3) need. For a
reimplementation this is a documented accuracy trade, not a technique to
copy blindly.

The microphone is emulated with a `.wav` file (`spu_load_fake_microphone_data`,
looked up per game then falling back to `microphone/microphone.wav`) or
white noise, sampled by position against `system->cycles`.

---

## 7. Audio is the frame limiter

`audio_sync`, called right after `update_spu` at VBlank when audio sync is
enabled:

```
while ((write_idx - read_idx) & 0xffff >= buffer_target * 3 / 4)
    delay_us(10);
```

The emulation thread blocks until the SDL callback has drained the ring
below three-quarters of the configured target. That is the pacing mechanism
for the whole emulator: real-time speed is not enforced by a timer but by
the audio device's consumption rate, which is what a listener actually
cares about. `system_frame_sync` (at line 215) layers a wall-clock check on
top for the frame-skip decision, but with audio enabled the audio buffer is
what holds the emulator at 60 Hz. With `SDL_AUDIODRIVER=dummy`, as in the
benchmark runs, both are inert — which is why the "Audio 0.00 ms" row in the
ablation is meaningless and the SPU cost had to come from `perf` instead.

---

## 8. Summary

| Technique | Effect |
|---|---|
| Mix once per frame at VBlank (`update_spu`, 1 call/frame) | Channel state loaded/stored 16× per frame, not 16× per sample |
| Fixed-point cycle→sample conversion with carried remainder | Exact long-run sample count, no drift, no float |
| Resampling folded into playback (`src[pos >> 32]`, 32.32 step) | No intermediate 32 kHz mix, no resampler pass |
| Pre-multiplied `vol_l` / `vol_r` shorts per channel | One multiply-add per channel per sample per side |
| Source resolved to a host pointer at key-on | Zero address translation in the sample loop |
| Dirty-bit journaling of register writes | ~5 channel recomputes per frame against ~8,000 register writes |
| ADPCM decoded 8 samples (one word) at a time into a 64-entry ring | <4 instructions per decoded sample; PCM-identical mix loop |
| Loop-point predictor snapshot | Looped ADPCM costs the same as looped PCM |
| PSG as 8-entry tables, noise as a precomputed 32 K LFSR table | Synthesis becomes PCM playback |
| Auto-vectorised clamp, no hand-written kernels | The only bandwidth-bound stage is the only SIMD stage |
| Lock-free 64 K ring with 16-bit indices | Producer/consumer interface is two integers |
| Capture units emulated for timing only | Correct control flow, cheapest possible data path |
| Audio buffer occupancy as the frame limiter | Real-time pacing tied to what the listener hears |

The recurring shape is once again the one from the renderers and the
recompiler: the sample loop has nothing left to decide because every decision
— format, rate, volume, address, loop state — was resolved at key-on, at
register-write time, or once per frame.
