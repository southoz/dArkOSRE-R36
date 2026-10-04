# The 3D software rasteriser

The DS has a fixed-function 3D unit that draws up to 2048 polygons into a
256×192 frame with per-pixel depth, texturing, four blend modes, toon shading,
edge marking, fog and anti-aliasing. DraStic reproduces it **entirely on the
CPU**, using no GPU at all, and it costs 1.08 ms/frame in Super Mario 64 DS on a
Cortex-A55 at 2 GHz — 32.6 % of all cycles. See
[00-method-and-measurements.md](00-method-and-measurements.md).

Emulating a GPU on a CPU is normally slow for two reasons: the framebuffer does
not fit in cache, and per-pixel work is full of unpredictable branches. DraStic's
renderer is organised almost entirely around removing those two costs. Three
ideas do the work:

1. **Bin the screen into cache-resident tiles**, so the framebuffer working set
   never leaves L1/L2.
2. **Decide everything that can be decided per-polygon at bin time**, so the
   inner loops have no decisions left in them.
3. **Process a span one *stage* at a time rather than one *pixel* at a time**, so
   every stage is a flat SIMD loop over an array and every conditional becomes a
   mask.

Before any of this runs, the guest's command stream has to reach the
rasteriser — and it does not do so at write time. GXFIFO writes are *logged*
and replayed once at VBlank, with the vertex transform run as a batch over the
result; `GXSTAT` reads replay the log on demand so the deferral stays
observable-correct. That is the geometry engine's own technique and is
described in [04 §6](04-scheduler-deferral-and-memory.md); everything below
starts from the transformed polygon list it produces.

---

## 1. Binning: twelve tiles of sixteen scanlines

`video_3d_bin_polygons_1x` sorts every polygon into **12 bins of 16 scanlines**
(12 × 16 = 192, the screen height). Each bin is a list of 16-bit polygon indices
with a count word, cleared at the top of the function at strides of `0x1004`.

Each bin's working buffers are:

| Buffer | Size |
|---|---|
| Colour | 256 px × 16 lines × 4 B = **16 KB** |
| Depth / attributes | 256 px × 16 lines × 4 B = **16 KB** |
| Polygon-ID stencil (translucent pass) | 256 × 16 = 4 KB |

So the entire live rasterisation footprint for a tile is about **32–36 KB** —
sized to the A55's 32 KB L1D and comfortably inside L2. Every depth read, depth
write, colour read and colour blend during rasterisation hits cache. Only the
resolve step at the end of the bin touches memory that is not already hot, and it
touches it exactly once, sequentially.

This is precisely the strategy a mobile GPU uses (tile-based deferred rendering),
implemented in software for the same reason: bandwidth, not arithmetic, is the
binding constraint.

### The bin mask is computed without branches

A polygon's bin membership is derived from its vertical extent with one
shift pair:

```
mask = (0xfff >> (11 - ((ymax - 1) >> 4))) & (0xfff << (ymin >> 4))
```

producing a 12-bit set of bins the polygon touches, which is then tested bit by
bit to append the polygon index to each list. No loop over bins, no comparisons
per bin — two shifts and an AND.

### Why twelve

12 divides evenly by 1, 2, 3, 4, 6 and 12. `video_3d_render_bins_1x` computes
each thread's share as `12 / thread_count`, so a 1-, 2-, 3-, 4-, 6- or 12-core
device gets a perfectly even split with no remainder handling. On the RK3566 the
split is four ways — the main thread plus three `video_3d_render_thread` workers
— three bins each. Measured sample counts for the three workers on the
benchmark run were 1,870 / 1,510 / 1,318, i.e. reasonably balanced.

---

## 2. Per-polygon specialisation decided at bin time

This is the renderer's central trick, and the reason it has 133 hand-written
assembly kernels.

### Detecting uniform attributes with AND/OR

While binning, DraStic accumulates both the bitwise **AND** and the bitwise **OR**
of each per-vertex attribute across all of a polygon's vertices. If the AND
equals the OR, every vertex had the same value — the attribute is **constant over
the polygon**:

```
for each vertex v:
    and_u &= v.u;   or_u |= v.u
    and_v &= v.v;   or_v |= v.v
    and_z &= v.z;   or_z |= v.z
...
if (or_v != and_v)  flags |= ...      /* v varies  */
if (or_u != and_u)  flags |= ...      /* u varies  */
```

Two instructions per vertex per attribute, once per polygon, and the result
selects a rasterisation kernel that **skips interpolation entirely** for that
attribute. Hence the `_constant` kernel family:
`render_polygon_depth_compare_less_than_constant_asm`,
`render_polygon_setup_perspective_steps_w_constant_asm`,
`render_polygon_alpha_combine_depth_fog_constant_asm`, and so on.

This matters enormously in practice because DS scenes are full of screen-aligned
quads, flat-shaded geometry, billboards and UI surfaces where W, Z, U or V really
are constant. `render_polygon_depth_compare_less_than_constant_asm` is 2.28 % of
cycles in SM64DS — more than the non-constant variant — so the specialised path
is not an edge case, it is the common case.

The results are packed into the polygon's descriptor word along with its maximum
Y and its top-vertex index, so the rasteriser reads one 32-bit word and knows
which kernels to call.

### The specialisation matrix

Rather than branch inside a loop, DraStic enumerates the combinations:

| Family | Variants | Selected on |
|---|---|---|
| `generate_texture_addresses_*` | **9** | U wrap-mode × V wrap-mode (wrap/clamp/flip each) |
| `alpha_combine_*` | **8** | depth-write × fog × constant-alpha |
| `resolve_bin_*` | **10** | edge-marking × fog mode × gap-filling |
| `depth_compare_*` | 4 | less-than/equal × constant/interpolated |
| `load_texels_*` | paletted / direct / 4bpp | texture format |
| `modulate` / `decal` / `toon_*` / `shade_untextured` | — | polygon blend mode |
| `_1x` / `_4x` | 2 whole families (64/65 symbols) | internal resolution |

Nine texture-address kernels exist because the DS's `TEXIMAGE_PARAM` has
independent repeat and flip bits per axis; writing one kernel with the modes
tested per pixel would put two unpredictable branches in the innermost loop.
Writing nine kernels puts zero.

### The resolve is fused

Post-processing — edge marking, fog, gap filling, conversion to the output
format, and the write from the tile buffer to the scanline buffer — is **one pass
per bin**, chosen once from a 3-bit key built from the display-control register:

```
key = (edge_marking << 2) | fog_mode
  → video_3d_resolve_bin_asm_1x
  → video_3d_resolve_bin_edge_mark_1x
  → video_3d_resolve_bin_fog_full_1x
  → video_3d_resolve_bin_fog_alpha_1x
  → video_3d_resolve_bin_edge_mark_fog_full_1x
  → video_3d_resolve_bin_edge_mark_fog_alpha_1x
```

A generic implementation would read and write the tile buffer once per effect.
The fused versions read it once, do all enabled effects in registers, and write
once.

---

## 3. The span pipeline: stages over arrays, not branches over pixels

### Spans are batched to 256 pixels before any of it runs

This is the part that makes the rest affordable, and it is visible in
`render_polygon_setup_1x`, the caller of the flush.

Once a polygon's edges have been walked into a per-scanline table of span
lengths, `render_polygon_setup_1x` does not render a scanline at a time. It
walks that table accumulating a **span count** and a **running pixel total**,
skipping zero-length rows, and compares the running total against `0x100`:

```
    add   w8, w6, w19          ; running total + this span's length
    cmp   w8, #0x100           ; 256 pixels
    b.hi  <flush and restart>
    add   w22, w22, #0x1       ; else take this span into the batch
```

When the batch would exceed **256 pixels** it calls `render_polygon_flush_1x`
once for everything accumulated so far — passing the span-table pointer, the
first scanline, the *span count* and the *total pixel count* — then advances
the scanline cursor by the span count and starts a new batch from the current
span. A polygon that fits in 256 pixels is flushed exactly once, for all of
its scanlines together.

So the unit of work is neither a pixel nor a span nor a scanline. It is **a
batch of up to 256 pixels drawn from consecutive scanlines of one polygon**,
and `flush_1x` loops over the spans inside it.

### Which is why the kernels can be so specialised

`render_polygon_flush_1x` is 628 instructions containing **39 direct calls and
not one indirect call** — no function pointers, no dispatch table. The
alternatives sit at distinct call sites and a branch picks one:
`depth_compare_less_than_asm` or `_less_than_constant_asm` or `_equal_asm` or
`_equal_constant_asm`; `setup_perspective_steps_asm` or
`..._w_constant_asm`; `load_texels_asm` or `load_texels_paletted_asm`; three
different writebacks. A single pass through the function executes perhaps eight
to twelve of the thirty-eight.

All of that selection — every branch, and the texture-address thunk below —
happens **once per 256-pixel batch**. That is the whole economic argument for
133 hand-written kernels. The dispatch is not cheap in itself; it is simply
amortised over two hundred and fifty-six pixels and several scanlines.

The texture-address selection shows the same shape. There are nine wrap-mode
kernels and **none of them has a direct caller**: `generate_texture_addresses`
is a 44-instruction thunk that reads the wrap nibble out of the polygon,
compares its way down four bits, and **tail-branches** (`b`, never `bl`) to the
right one:

```
    ldrh  w6, [x0, #2]
    and   w6, w6, #0xf
    ...
    b     <..._wrap_wrap_asm>
```

Five compares and a jump, once per batch, to remove two unpredictable branches
from an inner loop that will run up to 256 times.

### The stages

Within a batch, `flush_1x` does **not** loop over pixels. It loops over
*stages*, each of which is a separate NEON kernel applied to the whole batch:

```
setup_perspective_steps        →  per-span setup
interpolate_w / interpolate_z  →  array of W or Z
load_depth                     →  array of existing depths
depth_compare_*                →  BYTE MASK + surviving-pixel COUNT
setup_rgb_interpolants
interpolate_rgb                →  array of colours
setup_uv_interpolants
interpolate_uv                 →  arrays of U, V
generate_texture_addresses_*   →  array of texel addresses
load_texels_*                  →  array of texels
modulate / decal / toon_*      →  array of shaded colours
alpha_test / alpha_id_test     →  refined mask
alpha_blend / alpha_combine_*  →  array of blended colours
apply_fog
mark_edges
writeback_*                    →  masked store into the tile buffer
```

This is a structure-of-arrays reorganisation of what is conventionally an
array-of-structures inner loop, and it is what makes the renderer vectorisable at
all. Each stage is a flat loop over contiguous `int32` arrays with no control
flow, which NEON handles at eight pixels per iteration; the per-pixel decisions
that would have been branches become **byte masks** consumed by later stages.

### The attribute pipeline is flattened until the span boundaries vanish

The interpolation is three stages, and only the middle one knows what a span
is.

**1. Per-span endpoints, in C.** `render_polygon_edge_interpolate_x_c` (144
instructions, no divides, no SIMD) walks the edges once per polygon and leaves
each span's starting value and step in arrays indexed by span.

**2. Broadcast them per pixel, in assembly.**
`render_polygon_setup_rgb_interpolants_asm` is two nested loops -- the outer
over *spans* (`subs w2, w2, #1`), the inner over pixels eight at a time. It
loads a span's base and step with `ld1r` (load-replicate), `dup`s them across
vectors, and does nothing but **store**:

```
    ld1r  {v0.2s}, [x4], #4        ; this span's base
    dup   v4.4s, v0.s[0]
    st1   {v4.4s-v5.4s}, [x9], #32 ; eight pixels of it
    st1   {v18.8h}, [x1], #16
    subs  w3, w3, #0x8
    b.gt  ...                      ; inner: pixels
    subs  w2, w2, #0x1
    b.ne  ...                      ; outer: spans
```

There is no arithmetic in the inner loop at all. It expands per-span constants
into per-pixel arrays: a 32-bit accumulator base and a 16-bit step, per
channel.

**3. One flat pass over the whole batch.**
`render_polygon_interpolate_rgb_asm` then has **no span loop** -- just
`subs w3, w3, #8` over the batch's total pixel count. It reads the per-pixel
factor, the base and the step, and does `base + step * factor` with a widening
multiply-accumulate:

```
    ld1   {v0.4s}, [x2], #16       ; the factor, per pixel
    ld1   {v1.4s-v2.4s}, [x7], #32 ; base
    ld1   {v7.8h}, [x1], #16       ; step
    smlal v1.4s, v7.4h, v0.4h
    shrn  v18.4h, v1.4s, #16
    shrn  v18.8b, v18.8h, #2
    st1   {v18.8b}, [x0], #8
```

Three channels per iteration, eight pixels at a time, straight through the
batch. **The span structure has been flattened away before the expensive stage
runs.** That is what the batching is really for: not just amortising a call,
but turning a ragged set of 13-to-35-pixel spans into one contiguous vector
loop.

`render_polygon_setup_uv_interpolants_asm` is the same shape for texture
coordinates.

> **Why the flattening does not port to us, measured.** The planes are the
> whole cost of the scheme, and DraStic pays it because it has to: its spans
> arrive from a table that `edge_interpolate_x_c` filled, so materialising
> per-pixel base and step is simply how the data reaches the kernel. We
> compute each span's endpoints inline and hand them to `span_attrs5n` in
> registers, so the same planes are pure added traffic. Probed on the
> batching branch by writing the six planes and reading none of them:
> **+0.63 % on sm64 and +1.72 % on etody, slower in 3 of 3 paired reps** —
> a floor the flat pass would have to beat before it broke even. The
> perspective ramp below is a different matter and does port.

### The constant-W perspective factor is a ramp, not a division

`render_polygon_setup_perspective_steps_w_constant_asm` shows how far the
uniformity test pays off. With W constant the factor is *linear in x*, so the
kernel needs no division anywhere: it multiplies a constant iota vector
(0,1,2,3...) by the span's step once, then the inner loop is a single add.

```
    ld1   {v0.4s-v1.4s}, [x6]      ; iota_u32_value: 0,1,2,3,4,5,6,7
    dup   v2.4s, w5                ; this span's step
    mul   v4.4s, v0.4s, v2.4s
    shl   v3.4s, v2.4s, #3         ; step * 8
    shrn  v6.4h, v4.4s, #16        ; <- inner loop starts
    st1   {v6.8h}, [x0], #16
    add   v4.4s, v4.4s, v3.4s      ; forward difference, no divide
    subs  w4, w4, #0x8
```

Outer loop over spans again. Per pixel it costs a narrow, a store and an add.

### Masks and counts

`render_polygon_depth_compare_less_than_asm` is a good example of the whole
style:

```
    ld1   {v2.4s-v3.4s}, [x2], #32     ; 8 existing depths
    ld1   {v0.4s-v1.4s}, [x1], #32     ; 8 new depths
    bic   v2.4s, #0xff, lsl #24        ; depth is 24-bit; top byte is attributes
    bic   v3.4s, #0xff, lsl #24
    cmhi  v6.4s, v2.4s, v0.4s          ; compare, 8 lanes
    cmhi  v7.4s, v3.4s, v1.4s
    xtn   v16.4h, v6.4s                ; narrow 32-bit lanes ...
    xtn2  v16.8h, v7.4s
    xtn   v17.8b, v16.8h               ; ... to one mask BYTE per pixel
    sub   v18.8b, v18.8b, v17.8b       ; mask bytes are 0xFF = -1: accumulate count
    st1   {v17.8b}, [x0], #8
```

ending with `uaddlv` to total the count and a store of it to the caller. Two
things come out of one pass: the per-pixel mask, and **how many pixels survived**,
so subsequent stages can be skipped entirely when a span is fully occluded.

The tail is handled by *shifting the mask* (`ushl d22, d17, d20`) so partial
final groups contribute the right amount to the count — no scalar epilogue, no
branch.

---

## 4. Written for an in-order core

Several kernels only make sense as scheduling for a Cortex-A55, which cannot
reorder around a load.

**Texel fetch is deliberately scalar and eight-way unrolled.** ARMv8-A NEON has
no gather instruction, so texture lookup must be scalar. Rather than a short loop,
`render_polygon_load_texels_paletted_asm` issues eight *independent* dependency
chains back to back:

```
    ldp  w5, w6, [x1], #8      ldp w7, w8, [x1], #8   ...   ; 8 addresses
    ldrb w5, [x2, w5, uxtw]    ldrb w6, [x2, w6, uxtw] ...  ; 8 palette indices
    ldr  w5, [x3, w5, uxtw #2] ldr w6, [x3, w6, uxtw #2] ... ; 8 palette entries
    stp  w5, w6, [x0], #8      stp w7, w8, [x0], #8    ...  ; 8 results
```

Each texel needs two dependent loads. On an out-of-order core the hardware would
overlap them; on an A55 it will not, so the code does it by hand — eight chains
in flight means the L1 hit latency of one is covered by the issue of the next.
This kernel is 2.04 % of cycles and would be several times that written as a
loop.

**The vector kernels are software-pipelined.** The depth-compare loop above loads
the *next* iteration's data before storing the current iteration's result, so the
load latency overlaps the narrowing chain.

**Fixed point throughout.** `render_polygon_interpolate_w_asm` uses
`smull` + `shrn #15`; there is no floating point anywhere in the rasteriser. The
A55's FP is adequate but integer SIMD is wider and the DS's own arithmetic is
fixed-point anyway, so this is both faster and more accurate to the hardware.

**One call covers a whole batch.** The interpolation kernels take a table of
per-span lengths and loop over spans internally, rewinding pointers between them
(`add x2, x2, w6, sxtw #1`), rather than being called once per span. The batch
is the 256-pixel group assembled by `render_polygon_setup_1x` (§3), so for most
polygons this is genuinely one call for the entire polygon, and for a large one
it is a handful.

---

## 5. The texture cache costs 0.04 %

Texture cache lookup is hoisted out of the rasteriser entirely. It happens
**once per polygon, during binning**, and is memoised against the previous
polygon's parameters:

```
if (tex_params != last_params || tex_palette != last_palette) {
    resolved = texture_cache_lookup(...);
    last_params = tex_params;  last_palette = tex_palette;
}
polygon->texture = resolved;
```

Because games submit geometry grouped by material, consecutive polygons almost
always share a texture, so the one-entry memo hits nearly always. The resolved
pointer is stashed in the polygon record and the rasteriser never looks anything
up. `perf` measures the entire texture cache at **0.04 %** of cycles; the qemu
instruction profile puts it at 1,248 instructions per frame in SM64DS.

The lesson is not that the cache is clever but that the *lookup* was moved to the
coarsest granularity where it is still correct — per polygon, not per span and
certainly not per pixel.

---

## 6. Two passes and a stencil

Each bin is rasterised twice, matching the DS's own rules:

1. **Opaque polygons**, from the opaque sort list.
2. **Translucent polygons**, from a separate list, preceded by clearing a
   per-pixel polygon-ID stencil to `0xff`
   (`render_polygon_set_buffer8_asm(buf, 0xff, 0x100)` over 16 lines).

The stencil implements the DS rule that a translucent polygon does not blend
against another fragment of the *same* polygon ID —
`render_polygon_alpha_id_test_asm` is the kernel that enforces it. Keeping it as
a separate 4 KB buffer inside the tile keeps that test in cache too.

The clear itself is worth noting: the rear plane is written as
`0x8000000080000000` pairs, sixteen bytes at a time, or — when `DISPCNT` selects
a rear-plane bitmap — read from VRAM through `texture_cache_build_pixel_embedded_alpha`
with the scroll register applied, which is the awkward path most emulators get
wrong.

---

## 7. Every kernel exists twice

There are **133 `_asm` kernels and 169 `_c` kernels**, in matched pairs:
`render_polygon_modulate_asm` / `render_polygon_modulate_c`,
`video_3d_resolve_bin_asm_1x` / `video_3d_resolve_bin_c_1x`, and so on.

The C versions are not dead code — some are called directly
(`render_polygon_edge_interpolate_x_c` is 1.18 % of instructions in SM64DS, and
`render_polygon_mark_edges_c` is called from the flush path). But the pattern is
consistent enough to read as a deliberate discipline: a portable C reference
alongside each hand-written kernel, so the assembly can be validated against it
and so the renderer degrades rather than fails on a platform without the
hand-written path.

For a reimplementation this is the important structural lesson. The C kernel is
the specification; the assembly kernel is an optimisation that must produce
bit-identical output. Building the pair from the start makes the SIMD work
testable.

---

## 8. Summary

| Technique | Effect |
|---|---|
| 12 bins × 16 lines, 32 KB working set | Rasterisation never leaves cache |
| Branch-free 12-bit bin mask | Binning costs two shifts per polygon |
| 12 bins ÷ thread count | Even split on 1/2/3/4/6/12 cores, no remainder |
| AND/OR uniformity test per polygon | Selects kernels that skip interpolation |
| 9 / 8 / 10-way kernel specialisation | Zero unpredictable branches in inner loops |
| Fused resolve (edge + fog + convert + store) | One pass over the tile instead of four |
| Spans batched to 256 px across scanlines before the pipeline runs | Kernel selection is paid once per 256 pixels, not once per scanline |
| Per-span interpolants broadcast into per-pixel arrays | The interpolation kernel has no span loop: one flat pass over the batch |
| Constant-W perspective factor as an iota ramp | No division at all; one add per vector |
| Stage-at-a-time span pipeline (SoA) | Every stage vectorises; branches become masks |
| Mask + surviving-pixel count from depth test | Later stages skipped when a span is occluded |
| Eight-way unrolled scalar texel gather | Covers load latency on an in-order core |
| Texture lookup hoisted to bin time, memoised | Texture cache costs 0.04 % |
| Paired `_c` / `_asm` kernels | The assembly is testable against a reference |

The theme is the same as in [the recompiler](01-arm-to-aarch64-jit.md): decisions
are moved to the coarsest granularity at which they are still correct — per
frame, per bin, per polygon, per span — so that the per-pixel code has nothing
left to decide.
