# melonDS Rendering Internals

Companion to `nintendo-ds-graphics-rundown.md`. Where that document describes what the hardware *is*, this one describes what it takes to **reproduce** it — which turns out to be the more revealing angle, because the emulator has to confront every quirk the hardware documentation glosses over.

## Sources, ranked by trustworthiness

| Source | What it is | Notes |
|---|---|---|
| **Arisotura, "The DS GPU and its fun quirks"** (Oct 2018) — <https://melonds.kuribo64.net/comments.php?id=56> | Primary. Written by melonDS's lead dev. | The single best free document on DS rasterisation behaviour. Everything below marked *(Arisotura)* comes from here. |
| **melonDS source** — <https://github.com/melonDS-emu/melonDS> | GPLv3. `GPU3D.cpp`, `GPU3D_Soft.cpp`, `GPU3D_OpenGL.cpp`, `GPU3D_Compute.cpp`, `GPU2D.cpp` | The actual ground truth. Heavily commented. |
| **GBATEK** (Martin Korth) — <https://problemkaputt.de/gbatek.htm> | Register-level hardware reference | What melonDS is implemented *against*. |
| **DeepWiki melonDS pages** — <https://deepwiki.com/melonDS-emu/melonDS/3-graphics-system> | AI-generated wiki over the repo | Useful as a *map* with line-number citations into the source. Machine-generated — verify anything load-bearing against the code it links to. |

Note the licensing asymmetry: melonDS is GPLv3, Copetti's article is CC BY 4.0, Arisotura's blog is uncredited-but-public. Relevant if you're pulling any of this into your own work.

---

## 1. Code structure

```
GPU                    — coordinator: VRAM mapping, display timing, hosts both engines
├── GPU2D::Unit  ×2    — instances A and B (Main and Sub)
├── GPU3D             — geometry engine (always software, no backend choice)
└── Renderer3D        — abstract base for the rasteriser backend
    ├── SoftRenderer3D    (GPU3D_Soft.cpp)     — reference, CPU
    ├── GLRenderer3D      (GPU3D_OpenGL.cpp)   — OpenGL, fixed-function rasteriser
    └── ComputeRenderer3D (GPU3D_Compute.cpp)  — OpenGL 4.3+, manual raster in compute shaders
```

**The geometry engine is never hardware-accelerated.** It lives entirely in `GPU3D.cpp` and runs in software regardless of which backend you pick. Only rasterisation is swappable. This makes sense once you know the geometry stage is all fixed-point integer maths — there's nothing a GPU would do better.

`VRAMCNT` register writes drive a set of maps (`VRAMMap_ABG`, `VRAMMap_AOBJ`, etc.) that track which of the nine banks is currently serving which purpose. This mirrors the hardware's bank-mapping flexibility described in the main document.

---

## 2. Geometry pipeline, stage by stage

*(Arisotura)* — all arithmetic is **fixed-point integer**. The DS has no FPU.

1. **Transform & lighting** — 4×4 matrix transforms on vertices and texture coordinates; optional per-vertex lighting. The matrix stack is effectively a hardware `glPushMatrix()`. One nonstandard detail: **texture coordinates are in texels, not normalised** — 1.0 means one texel.
2. **Polygon setup** — triangles, quads, tri-strips, quad-strips. **Quads are native**, never decomposed. This is the single biggest obstacle to hardware-accelerated rendering, since modern GPUs are triangle-only.
3. **Culling** — standard front/back-face elimination.
4. **Clipping** — against 6 planes. Doesn't *create* polygons, but each plane can **add** a vertex, so a clipped polygon can end up with **up to 10 vertices**. The rasteriser therefore has to handle arbitrary vertex counts.
5. **Viewport transform** — X/Y to screen space; Z scaled into the depth buffer's 24-bit range. W gets an unusual treatment: it's **normalised into 16 bits** by shifting right by 4 until all of a polygon's W values fit, or left if all are under `0x1000` — a precision-preserving hack for interpolation.
6. **Sorting** — translucent polygons last, then **sorted by Y coordinate** (mandatory for opaque, optional for translucent).

> **Why the 2,048 limit exists:** step 6. Sorting requires storing every polygon first. The ceiling is a consequence of the sort buffer, not an arbitrary cap. There are dedicated internal banks for polygons and vertices, and a register reporting current occupancy.

---

## 3. Rasterisation — where the weirdness lives

### 3.1 Scanline, not tiled

*(Arisotura)* Modern GPUs rasterise in tiles with triangle-optimised algorithms. The DS is a **scanline renderer**, and it has to be: it must run in lockstep with the per-scanline 2D tile engines. The 48-scanline buffer exists to absorb overload on individual scanlines, not to decouple the stages.

melonDS's software renderer mirrors this with three per-pixel buffers:

| Buffer | Contents |
|---|---|
| `ColorBuffer` | 32-bit RGBA |
| `DepthBuffer` | 32-bit (Z or W) |
| `AttrBuffer` | Polygon ID (6 bits), fog flag, alpha (5 bits), and more |

### 3.2 One span per scanline — with a twist

This is the quirk noted in the main document, but Arisotura's explanation of *how* it goes wrong is the useful part:

The rasteriser is a **convex** polygon filler accepting arbitrary vertex counts. It picks left and right edges starting from the topmost vertices and walks them. Given a self-crossing "butterfly" quad, it fills incorrectly — but not randomly. It:

- fills until it reaches the end of an edge,
- looks for the next vertex on that edge, **knowing not to pick vertices above the current one**,
- **knows that left and right edges have swapped**, and continues.

So the failure mode is deterministic and reproducible. Games that trip it get consistent wrong output on hardware, which means emulators must reproduce the wrongness exactly.

### 3.3 Interpolation without barycentrics

Barycentric interpolation works for triangles. The DS supports up to 10-gons, so it does something else:

1. For the current scanline, find the vertices bracketing each edge.
2. Use edge slopes to find the span endpoints; interpolate vertex attributes at those two points — **based on vertical position within the edge, or horizontal position if the edge is X-major**.
3. For each pixel in the span, interpolate between those two endpoint values by X position.

It's perspective-correct in principle (`1/W` logic in melonDS's `Interpolator` class, which has both Linear and Perspective modes), but with "fun shortcuts and quirks" in Arisotura's phrasing.

### 3.4 Fill rules are per-pixel, not per-polygon

**This is the detail that breaks hardware rendering.** Opaque and translucent polygons follow different fill rules — and the rules are applied **per pixel**. A polygon flagged translucent can contain opaque pixels, and those pixels follow the *opaque* rules.

On a modern GPU this requires separate rendering passes. There's no way to express it in one draw call.

---

## 4. The attribute buffer problem

The DS's attribute buffer tracks, per pixel:

- Opaque polygon ID (6 bits) **and** translucent polygon ID, separately
- Whether the pixel was translucent
- Whether it should receive fog
- **Whether it came from a front- or back-facing polygon**
- Whether it sits on a polygon edge

A typical GPU stencil buffer is **8 bits**. That is nowhere near enough, and it's the central engineering problem in every DS hardware renderer.

What the fields are used for:

| Field | Purpose |
|---|---|
| Opaque polygon ID | Drives edge marking |
| Translucent polygon ID | A translucent pixel is **discarded** if its ID matches the existing translucent ID in the buffer |
| Both IDs | Control where shadows land — e.g. a shadow that covers the floor but not the character standing on it |
| Fog flag | Gates the fog pass; update rule differs for opaque vs translucent incoming pixels |
| Front-facing flag | See below |

When drawing translucent pixels, the existing **opaque** polygon ID and the edge flags from the last opaque polygon are **preserved**, not overwritten.

### The front-facing depth quirk

Arisotura calls this one "a bastard." The `less than` depth comparison **accepts equal values** when drawing a front-facing polygon over opaque back-facing pixels.

*Sands of Destruction* depends on it: the game draws screens where all Z values are zero, and without this quirk emulated, UI elements simply vanish. The apparent design intent was that an object's front face should always win over its back face even on completely flat geometry.

There are other depth-test edge cases that remain undertested and undocumented.

---

## 5. Final passes, in order

1. **Edge marking** — pixels with edge flags get a colour from a table indexed by opaque polygon ID. Two side effects worth knowing: a translucent polygon drawn over an opaque one **still shows the opaque polygon's marked edges**; and because of how clipping works, **polygon/screen-border intersections also get coloured** (visible in *Picross 3D*).
2. **Fog** — per-pixel, depth indexes a 32-entry density table, result blends toward `RenderFogColor`. Applied only where the fog flag is set.
3. **Anti-aliasing** — coverage values computed during rasterisation from edge slopes, then blended in this pass. Opaque edges only.

**Interaction rule:** if edge marking and AA would both apply to the same pixel, you get **edge marking only, at 50% opacity**.

**Bonus quirk:** wireframe polygons are rendered by *only filling the edges*. No separate wireframe path — it falls out of the edge machinery.

### Toon and highlight shading

`RenderToonTable` holds 32 16-bit colours.

- **Toon**: the computed vertex colour's brightness indexes the table, and the table colour **replaces** it.
- **Highlight**: the table colour is **added** to the texture colour, giving a metallic/glossy look.

Same table, two blend semantics. The main document treats toon shading as a single feature; it's actually a pair.

---

## 6. Depth buffering: Z vs W

| Mode | Behaviour |
|---|---|
| **Z-buffer** | Z transformed into 24-bit range, interpolated **linearly** across the polygon |
| **W-buffer** | W used **as-is**, interpolated with **perspective correction** |

*(Arisotura)* Modern GPUs typically store `1/W`. The DS doesn't, because reciprocals in fixed-point arithmetic are a bad idea. This is a case where the DS's choice is a direct consequence of having no FPU.

---

## 7. Threading model (software renderer)

melonDS decouples 3D rendering from the emulation loop using three semaphores:

- `Sema_RenderStart` — a frame of geometry is ready in `RenderPolygonRAM`
- `Sema_ScanlineCount` — incremented per completed scanline, letting the **2D engine catch up and pull lines for composition before the frame finishes**
- `Sema_RenderDone` — all 192 lines complete

That middle semaphore is the emulator faithfully reproducing the hardware's scanline lockstep. It's not an optimisation; it's required for correctness in games that read partial 3D output.

Timing notes from the `GPU3D.cpp` header comments (useful if you're chasing frame-timing bugs):

- Vertex/polygon RAM is filled when a polygon is complete, **after culling and clipping**
- The bank used by the renderer is emptied at **scanline ~192**
- Banks are swapped at **scanline ~194**

---

## 8. Why hardware rendering is hard — and the compute-shader answer

Arisotura's 2018 conclusion, which has aged well:

> The choice of API (OpenGL vs Vulkan vs D3D) is **irrelevant** to the difficulty. Modern GPUs have overwhelming raw power for 2,048 polygons at 256×192. The problem is entirely the **rasterisation quirks** — native quads, per-pixel fill rules, the oversized attribute buffer, the depth-test edge cases.

The three backends and what they trade:

| Backend | Approach | Trade-off |
|---|---|---|
| **Software** | Scanline rasteriser on CPU | Near pixel-perfect. Fast enough on almost any CPU; scales terribly with internal resolution. |
| **OpenGL** | Fixed-function rasteriser + GLSL shaders for DS depth modes. Uses `ClearShaderBitmap` — a fullscreen quad blitting DS clear values into GL colour/depth buffers before polygon rendering. | Arisotura targeted **~90% compatibility while being fast**. Some features will never be implemented; games needing them fall back to software. Enables up to **8× internal resolution**. |
| **Compute (GL 4.3+)** | Manual rasterisation in compute shaders: `InterpSpans` (edge interpolation) → `Binning` (assign polygons to screen tiles) → `Rasterize` (coverage/depth/colour) → `FinalPass` (tile composition) | Started as a **complete replication of the software renderer** on GPU. Originally deko3d for Switch, later ported to OpenGL. |

The compute renderer's origin is instructive for your purposes: per RSDuck, it was built **because the Switch has a weak CPU but a decent GPU**. CPU software rendering is "fine" for DS on most hardware — the compute path exists to make *upscaling* viable, since higher internal resolution scales badly on CPU.

That's a directly relevant data point for low-power ARM handhelds: if the CPU can hold native res, software rendering is the accuracy-optimal choice, and you only need a GPU path once you want to upscale.

---

## 9. The dual-screen 3D problem (still open as of recent builds)

The main document describes the mid-frame Main/Sub swap trick. Emulating it has been a long-standing melonDS bug: screens flicker between high- and low-resolution output, or the effect fails entirely.

The cause is the **display capture** feature. Captured frames must be at native **256×192** and must fit within emulated VRAM. Since capture is also used to verify the console is functioning, it **cannot simply be disabled** in an accuracy-first emulator. Reconciling native-res capture with an upscaled internal renderer is the actual difficulty.

Arisotura has described in-progress work on this alongside texture filtering, 2D layer/sprite filtering, hi-res rotation/scale, and AA. Check current release notes — this postdates my reliable knowledge and I haven't verified the present state.

---

## 10. Where to go next

- Read `GPU3D_Soft.cpp` directly. It's the reference implementation and the comments are candid about what's guessed vs. verified.
- Arisotura's earlier posts on specific subsystems — "the fancypants quads," "the viewport shito," "a fun rasterizer quirk," "the wonderful antialiasing implementation" — are linked from the main quirks post and go deeper on each.
- The compute renderer post: <https://melonds.kuribo64.net/comments.php?id=143>
- GBATEK's `DS 3D` sections for the register-level view of everything above.
- **noclip.website** implements a partial DS renderer for *Super Mario 64 DS* model viewing — a smaller, more readable codebase if melonDS is too much at once.
