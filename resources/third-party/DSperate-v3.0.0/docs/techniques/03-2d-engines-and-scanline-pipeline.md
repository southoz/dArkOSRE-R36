# The 2D engines and the scanline pipeline

The DS has two 2D engines, each with four background layers, 128 sprites,
two windows plus an OBJ window, mosaic, and a colour-effects unit — composited
per scanline, with every control register legally changeable mid-frame. DraStic
renders all of it on the CPU, one scanline at a time, on the **main emulation
thread**, and it is the single largest consumer of cycles in the emulator:
**1.96 ms/frame and 25.2 % of cycles** in Super Mario 64 DS, and the dominant
cost in any 2D game. See
[00-method-and-measurements.md](00-method-and-measurements.md).

That it costs more than the 3D engine surprises people. It should not: the 3D
engine draws whatever geometry the game submits, but the 2D engines composite
**up to ten layers across 2 × 256 × 192 pixels every single frame**, unconditionally.

The organising idea is different from the 3D renderer's. There, the win came from
tiling and from specialising kernels. Here it comes from a change of
representation: **for most of the pipeline, a pixel is one bit.**

---

## 1. Where it runs, and why it is per-scanline

`perf` call graphs put the whole 2D pipeline on the main thread, inside the
scheduler:

```
recompiler_entry_direct
  → execute_events
    → event_scanline_start_function
      → update_frame
        → video_render_scanlines
          → video_2d_render_scanlines
            → render_scanline
              → render_scanline_2d
                → render_scanline_bg → render_scanline_tiled_ext
                  → render_scanline_tiled_span_4bpp_asm
```

The call graph is accurate but misleading about *when*. The entry is the
VBlank scanline event, not every scanline event: in the common case
`update_frame` renders all 192 lines of engine A in one call while a worker
thread renders engine B. The 2D engines have no once-per-frame latch like the
3D engine — scroll registers, palettes, blend coefficients, window bounds and
even BG modes are routinely rewritten during HBlank, and raster effects depend
on that — so DraStic *journals* every mid-frame 2D write with the scanline it
happened on and replays the journal line by line as it renders. The pipeline
below therefore sees exactly the register state the guest had established at
each line, without having been invoked at each line. The mechanism is in
[04 §5](04-scheduler-deferral-and-memory.md).

What remains true is that each line must still be composited, in full, for two
engines, every frame. The response is to make each scanline extremely cheap.

---

## 2. One bit per pixel

The pipeline for a scanline is:

1. **Fetch** each enabled layer's pixels for this line into an index buffer, and
   simultaneously produce a **256-bit visibility mask** — one bit per pixel,
   set where the layer is non-transparent.
2. **Resolve priority** across all layers using nothing but boolean operations on
   those masks, producing per-layer "you are the top pixel here" and "you are the
   second pixel here" masks.
3. **Select** actual pixel values using the masks, 32 pixels at a time.
4. **Expand** to planar 6-bit colour channels and apply effects.
5. **Convert** to the output format.

Steps 1–3 are where the speed is. A 256-pixel scanline is 32 bytes as a bitmask —
**two NEON registers**. A boolean operation across an entire scanline is two
instructions.

### Building the masks

`render_scanline_set_visibility_4bpp_asm` builds the mask for a 4bpp layer.
4bpp packs two pixels per byte, and a pixel is transparent when its nibble is
zero:

```
    cmtst v17.16b, v0.16b, v26.16b    ; v26 = 0x0f : low nibble non-zero?
    cmtst v0.16b,  v0.16b, v27.16b    ; v27 = 0xf0 : high nibble non-zero?
    bit   v17.16b, v0.16b, v28.16b    ; merge per nibble order
    and   v17.16b, v17.16b, v29.16b   ; AND with {1,2,4,8,16,32,64,128}
    ...
    addp  v17.16b, v17.16b, v18.16b   ; pairwise-add tree packs flags to bits
    addp  v18.16b, v19.16b, v20.16b
```

The `and` with per-lane bit positions followed by an `addp` reduction tree is the
standard NEON "movemask" idiom — ARM has no `PMOVMSKB`, so the byte-per-pixel
comparison result is folded down to a bit-per-pixel mask in a few instructions.
There are variants for 4bpp, 8bpp and 12bpp, plus 16bpp direct-colour forms.

---

## 3. The bit-parallel priority encoder

This is the best single idea in the 2D engine.
`render_scanline_priority_encode_double_asm` walks the layers in priority order
and computes, for every one of 256 pixels simultaneously, **both** which layer is
on top **and** which layer is second — because the DS's colour-effects unit needs
the second layer as the blend source.

The entire inner loop is boolean:

```
    ld1  {v18.4s-v19.4s}, [x6]      ; this layer's 256-bit visibility mask

    bic  v20.16b, v18.16b, v0.16b   ; layer AND NOT already-covered
    bic  v21.16b, v19.16b, v1.16b   ;   → pixels where THIS layer is first
    st1  {v20.4s-v21.4s}, [x7]

    bic  v20.16b, v18.16b, v2.16b   ; layer AND NOT already-second
    bic  v21.16b, v19.16b, v3.16b
    and  v20.16b, v20.16b, v0.16b   ;   AND already-covered
    and  v21.16b, v21.16b, v1.16b   ;   → pixels where THIS layer is second
    st1  {v20.4s-v21.4s}, [x9]

    and  v20.16b, v0.16b, v18.16b   ; accumulate second-covered
    and  v21.16b, v1.16b, v19.16b
    orr  v2.16b,  v2.16b, v20.16b
    orr  v3.16b,  v3.16b, v21.16b
    orr  v0.16b,  v0.16b, v18.16b   ; accumulate first-covered
    orr  v1.16b,  v1.16b, v19.16b
```

Fourteen NEON instructions per layer resolves priority for **all 256 pixels at
once**. A scanline with six active layers costs under a hundred instructions to
fully resolve — first and second place, for every pixel — with no comparisons,
no branches, and no per-pixel work of any kind.

`priority_encode_double_obj_layer` is the same loop with two extra accumulators,
because sprites need to be tracked separately: a semi-transparent OBJ pixel
forces blending regardless of the blend-control register, so the OBJ first/second
masks are kept apart from the BG ones.

The `_single` variant is used when no blending is enabled and only the top layer
is needed, halving the work.

At the end, `mvn` and `bic` produce the backdrop mask — the pixels no layer
covered — for free from the accumulated coverage.

---

## 4. Turning masks back into pixels

`render_scanline_select_pixels_binary_asm` consumes a mask and merges two pixel
sources:

```
    ldr   w6, [x3]                   ; 32 pixels' worth of mask
    ld2r  {v2.8h-v3.8h}, [x3], #4    ; replicate the two mask halves
    cmp   w6, #0
    b.eq  skip_32_pixels             ; ← whole group empty: bump pointers only

    ld1   {v4.8h-v7.8h}, [x1], #64   ; source A, 32 pixels
    ld1   {v20.8h-v23.8h}, [x2], #64 ; source B, 32 pixels
    cmtst v16.8h, v2.8h, v0.8h       ; expand mask bits to per-lane masks
    ...
    bit   v4.16b, v20.16b, v16.16b   ; per-pixel select
    ...
    st1   {v4.8h-v7.8h}, [x0], #64
```

Thirty-two pixels per iteration, and — importantly — **a zero mask word skips the
whole group** with three pointer additions. Layers that cover only part of a line,
which is most of them, cost nothing where they are absent. This is the payoff for
having carried the mask around: it is both the selection input and a cheap
occupancy test.

---

## 5. Colour effects on planar channels

The DS blends in 5-bit-per-channel BGR555. Doing that on packed 16-bit pixels
means unpacking and repacking around every operation. DraStic unpacks **once per
scanline** into three planar byte arrays:

```
render_scanline_expand_6bit_split_asm:
    ld1   {v0.8h-v3.8h}, [x1], #64
    xtn   v4.8b,  v0.8h              ; red   = low byte
    shrn  v6.8b,  v0.8h, #4          ; green = middle
    shrn  v16.8b, v0.8h, #8          ; blue  = high
    shl   v4.16b, v4.16b, #1         ; 5-bit → 6-bit
    ushr  v16.16b, v16.16b, #1
    and   v4.16b, v4.16b, v18.16b    ; v18 = 0x3e
    ...
    st1   {v4.16b-v5.16b}, [x0], #32   ; R plane at +0x000
    st1   {v6.16b-v7.16b}, [x2], #32   ; G plane at +0x100
    st1   {v16.16b-v17.16b}, [x3], #32 ; B plane at +0x200
```

Three arrays of 256 bytes, one per channel, each value scaled to 6 bits (the
extra bit gives the DS's blend arithmetic the headroom it needs to round
correctly).

Now every effect — alpha blend, brighten, darken, the 3D alpha gather, master
brightness — is a byte operation on a flat array: **sixteen pixels per NEON
register per channel**, no unpacking anywhere. The whole
`render_scanline_color_effects_*` family (setup_darken, setup_brighten,
setup_blend, setup_alpha, apply, apply_offset) works in this representation, and
only `render_scanline_color_convert_*` at the very end packs back to RGB555 or
RGB8888 for output.

Measured: `expand_6bit_split_asm` is 1.68 % of cycles, `shade_asm` 1.71 %,
`color_convert_direct_32_1x_asm` 1.78 % — the conversions cost about as much as
the effects themselves, which is the sign that the representation is the right
one.

---

## 6. Tile fetch: `tbl` as a palette unit, `bit` as a flip unit

`render_scanline_tiled_span_4bpp_asm` is the hottest function in SM64DS by
instruction count (5.28 %) and 3.31 % of cycles. It fetches 4bpp tiled background
data, and it does two things worth stealing.

**Horizontal flip, branch-free, four tiles at a time.** A tilemap entry has an
H-flip bit. Rather than branch on it:

```
    cmtst v4.4s, v4.4s, v0.4s   ; test H-flip bit of 4 tilemap entries at once
    ushr  v3.16b, v2.16b, #4    ; build the flipped version:
    sli   v3.16b, v2.16b, #4    ;   swap nibbles within bytes
    rev32 v3.16b, v3.16b        ;   reverse bytes within each word
    bit   v2.16b, v3.16b, v4.16b  ; select flipped/unflipped per tile
```

Four tiles' worth of H-flip resolved with no branch at all.

**`tbl` as a 16-entry palette lookup.** The palette for a 4bpp tile is 16 BGR555
entries. DraStic loads it as **two 16-byte planes** — low bytes and high bytes —
with `ld2`, which lets NEON's byte-wise table lookup do the palettising:

```
    and   v3.16b, v2.16b, v1.16b     ; low nibbles  (v1 = 0x0f)
    ushr  v4.16b, v2.16b, #4         ; high nibbles
    zip1  v22.16b, v3.16b, v4.16b    ; interleave into pixel order
    zip2  v24.16b, v3.16b, v4.16b
    ld2   {v5.16b-v6.16b}, [x9]      ; palette as (lo-byte plane, hi-byte plane)
    tbl   v5.8b, {v5.16b}, v22.8b    ; look up low bytes
    tbl   v6.8b, {v6.16b}, v22.8b    ; look up high bytes
    st2   {v5.8b-v6.8b}, [x0], #16   ; re-interleave to 16-bit pixels
```

`tbl` is a 16-byte table lookup; a 4bpp palette is exactly 16 entries. Splitting
the palette into byte planes makes the two fit, and `ld2`/`st2` handle the
de-interleave and re-interleave for free as part of the memory access. Sixteen
pixels palettised per instruction pair.

There are matching kernels for 8bpp with normal and extended palettes
(`tiled_span_8bpp_normal_palette_asm`, `..._ext_palette_asm`), which cannot use
`tbl` (256 entries) and use gathers instead — hence
`render_scanline_palette_lookup_8bpp_asm` appearing separately at 2.76 % of
cycles.

---

## 7. Windows, mosaic, and the rest of the awkward parts

The same bitmask representation absorbs the features that usually make 2D
emulation slow:

- **Windows.** `render_scanline_generate_window_masks` builds each window's
  horizontal extent as a bitmask; `render_scanline_window_inhibit_masks_single` /
  `_double` / `_triple` (one per number of active windows) AND the layer
  visibility masks against them. Window clipping becomes one AND per layer.
- **Mosaic.** `render_scanline_apply_mosaic` and
  `render_scanline_apply_mosaic_visibility` apply it to both the pixels and the
  mask, before priority resolution, so nothing downstream knows mosaic exists.
- **Blank-layer elimination.** `render_scanline_disable_blank_layers_asm` drops
  layers whose masks came out empty, so the priority encoder does not iterate
  over them.
- **3D as a layer.** `render_scanline_set_3d_visibility` (1.11 % of cycles) turns
  the 3D engine's tile-resolved output into exactly the same kind of visibility
  mask as a BG layer, and `render_scanline_gather_3d_alpha_asm` extracts its
  per-pixel alpha for blending. The 3D engine is composited as just another
  layer, which is why nothing in §3 has to special-case it.
- **Display capture.** `render_scanline_capture_direct_asm` /
  `_capture_blended` / `_capture_direct_3d_asm` / `_capture_blended_3d` —
  four fused variants rather than a generic path, chosen once per scanline.

---

## 8. The same `_c` / `_asm` discipline

As in the 3D renderer, nearly every kernel exists twice —
`render_scanline_priority_encode_double_c` beside `..._asm`,
`render_scanline_select_pixels_binary_c` beside `..._asm`. Some C versions remain
live in the hot path (`render_scanline_obj_c` is 1.32 % of cycles in SM64DS and
6.20 % in Meteos — sprite fetch was evidently never converted), which is a useful
signal in itself: it shows the assembly conversion was applied where profiling
justified it and not elsewhere.

---

## 9. Summary

| Technique | Effect |
|---|---|
| Per-scanline pipeline, batched per frame with a write journal (04 §5) | Mid-frame register writes are exact, rendering is not per-line |
| 1 bit per pixel visibility masks | A whole scanline is two NEON registers |
| NEON movemask idiom (`and` + `addp` tree) | Byte comparisons fold to bitmasks cheaply |
| Bit-parallel priority encoder | First *and* second layer for 256 px in ~14 insns/layer |
| Zero-mask-word skipping in select | Partially-covering layers cost nothing where absent |
| Planar 6-bit channel split | Effects are byte ops, 16 px/register, no unpacking |
| `tbl` + `ld2`/`st2` as a 4bpp palette unit | 16 pixels palettised per instruction pair |
| `bit` + `rev32`/`sli` for H-flip | Four tiles flipped branch-free |
| Windows as mask ANDs | Clipping costs one instruction per layer |
| 3D output presented as an ordinary layer | No special cases in the compositor |
| Fused capture variants | One pass instead of a generic pipeline |

The through-line across all three documents is one idea applied at three scales:
**choose a representation in which the expensive decision has already been
made.** The recompiler chooses host registers so the guest's flags are already
correct. The 3D renderer chooses per-polygon kernels so the inner loop has
nothing to test. The 2D engine chooses one bit per pixel so that compositing an
entire scanline is a handful of boolean instructions.
