# BlocksDS Addendum — The Programmer's View

Fourth document in the set. Source: the **BlocksDS tutorial** (CC BY-SA 4.0), chapters on Sprites, Special 2D Effects, DMA, 3D Graphics, and Advanced 3D Techniques.

Where the other three documents describe what the hardware *is*, this one covers **what it costs and how it's actually driven** — the constraints that only show up once you're writing code against it. Several of these are absent from GBATEK because they're consequences of the hardware rather than register definitions.

**Coverage note:** the `Backgrounds` basic chapter is the one requested page I didn't pull; the register-level background material in the GBATEK addendum §4 covers the same ground structurally, and the sprites chapter states the background extended-palette workflow is analogous. Worth grabbing if you want the grit conversion flags for backgrounds specifically.

---

## 1. The constraint nobody else mentions: per-scanline sprite budget

> "There is a limit to the number of sprites that can be displayed in a horizontal line of the screen. The 2D engine has a limited time to draw sprites. Regular sprites take some time to be drawn, affine sprites take more time. If the graphics engine runs out of time, some sprites won't be displayed."

**128 sprites per screen is the OAM capacity, not a rendering guarantee.** There's a per-scanline time budget, and affine sprites cost more than regular ones, and double-size affine sprites cost more still. Exceed it and sprites silently vanish.

This is the 2D-side analogue of the 3D engine's 48-line buffer underflow, and it's the direct practical reason the 3D-as-2D technique (main document §4.6) exists. BlocksDS says so explicitly: if you want lots of transformed sprites, use the 3D engine instead.

### Affine sprite limits

- All 128 sprites can be rotated/scaled — but there are only **32 transformation matrices per engine**, shared across all of them. Multiple sprites can share an index (0–31).
- Rotation doesn't enlarge the sprite's canvas, so **rotating a 64×64 sprite 45° crops the corners**. The "double size" flag expands the canvas to 128×128 to fix this — at extra rendering cost.

### Sprite constraints worth knowing

- **Sprites can use only one palette** (unlike backgrounds' 16), unless extended palettes are enabled.
- Three types only: 16-colour tiled, 256-colour tiled, direct-colour bitmap. **There is no 256-colour bitmap sprite option.**
- Legal sizes are a fixed list: 8×8, 16×16, 32×32, 64×64, 16×8, 32×8, 32×16, 64×32, 8×16, 8×32, 16×32, 32×64. Nothing else.
- OAM is **512 bytes per engine** in practice (4 bytes × 128 sprites) — small enough that BlocksDS recommends refreshing all of it every frame regardless.

### The mapping-mode trade-off

The tile index → address calculation is `base + (index × entry_size)`, and entry size scales with how much VRAM you want reachable:

| Max accessible VRAM | Tile entry size |
|---|---|
| 32 KB | 32 bytes |
| 64 KB | 64 bytes |
| 128 KB | 128 bytes |
| 256 KB | 256 bytes |

**More reachable VRAM means coarser allocation granularity.** At 32 KB, your unit is one 8×8 16-colour tile. At 256 KB, the smallest allocatable unit is four 8×8 256-colour tiles. Rule of thumb: pick the mode matching what you've actually allocated, no more.

Bitmap sprites have their own modes: 128 KB (128-byte entries) or 256 KB (256-byte entries).

Engine asymmetry again: main engine can use banks **A, B, E, F, G** for sprites (16 KB up to 256 KB, flexible). Sub engine can only use **D and I** — so your choice is 128 KB or 16 KB, nothing between. The 256 KB mapping mode is useless on sub.

---

## 2. HBL-triggered DMA — the graphics trick worth having

You were right that this belongs here. Per-scanline register writes driven by DMA are the mechanism behind a whole class of DS effects.

**The configuration:**

```
DMA_SRC_INC        — advance source each copy
DMA_DST_FIX        — destination stays on one register
DMA_START_HBL      — fire at start of horizontal blank
DMA_REPEAT         — don't stop after the first copy
DMA_COPY_HALFWORDS | 1  — one halfword per scanline
```

Point the destination at a video register, the source at a 192-entry table, and you get a per-scanline value stream at near-zero CPU cost.

**Why DMA rather than an HBL interrupt:** the interrupt path costs a full handler dispatch *per scanline*. The DMA path costs one VBL handler dispatch per frame plus a few cycles per line. For an effect running on all 192 lines, that difference is substantial.

**Three gotchas that bite:**

1. **There's no "stop at end of frame."** You must stop and restart the channel yourself from a VBL handler each frame.
2. **The first copy happens at the HBL of line 0 — after line 0 has already been drawn.** So the VBL handler must write line 0's value directly, and DMA covers lines 1–191. Off-by-one here produces a one-line offset that's easy to misdiagnose.
3. **Cache coherency.** If you compute the table on the ARM9, it's sitting in the data cache and DMA can't see it. You must `DC_FlushRange()` before starting the transfer.

> **The cache point matters for emulation work specifically:** "emulators normally don't emulate the cache. If your copies work fine in an emulator but they show garbage data when running on real hardware, it's likely you're having issues with the cache."

That's a one-directional failure mode — code that's broken on hardware can look perfect under emulation. Worth remembering in both directions if you're comparing emulator output against a real unit.

**Other DMA constraints:** it cannot access ITCM or DTCM at all (so stack-resident data is unreachable, and the copy silently doesn't happen), and DMA copies block the CPU including interrupt handling unless the CPU is running entirely out of TCM.

Cearn's two articles are the deep reference: *DMA vs ARM9 - fight!* and *DMA vs ARM9, round 2: invalidate considered harmful* (both on coranac.com, archived).

### Documented uses of per-scanline DMA

- **Horizontal scroll per line** → the classic wave/parallax effect (write `REG_BG0HOFS`).
- **Circular windows** → write `REG_WIN0H` every scanline using a midpoint-circle algorithm, turning the rectangular window hardware into arbitrary shapes.

That second one is the genuinely clever application: the window hardware only does rectangles, but a rectangle whose left/right bounds change every scanline is any shape you want.

---

## 3. Window and blending details

### Windows

Two rectangular windows (0 and 1) plus the **object window**, which takes the shape of sprites. **Window 0 takes priority over window 1.**

Per window, you control each of the 4 background layers individually, all sprites as a single layer, and blending effects.

**Object window:** mark a sprite as a window mask, and **any pixel with palette index ≠ 0 is part of the mask**. The palette itself is never loaded — the sprite is never displayed, only its silhouette matters. Combined with the per-scanline trick above, this covers nearly any masking shape you'd want.

### Blending

`REG_BLDCNT` / `REG_BLDALPHA` / `REG_BLDY` select from **6 layers** — the 4 backgrounds, sprites, and the **backdrop colour**. Modes: alpha blend, fade to white, fade to black, or none.

The blend maths:

```
result = ((source × eva) + (destination × evb)) / 16     [clamped to 31]
```

- `eva + evb == 16` → normal alpha blending
- `eva + evb > 16` → **brighter than expected, giving a glow effect**
- `eva + evb < 16` → darker than expected

That's a deliberate, usable effect, not a bug — the DS's cheap bloom.

**Three blending paths that override each other:**

1. Register-based, whole-layer.
2. **Per-sprite alpha blending** (`SpriteMode_Blended`), which overrides the *mode* in `REG_BLDCNT` for that sprite but still uses its source/destination layer selection and `REG_BLDALPHA` values. So if `BLDCNT` says "fade to white for sprites," sprites flagged blended will alpha-blend while the rest fade.
3. **Bitmap sprite alpha**, which is mutually exclusive with (2). Bitmap sprites have no palette, so the palette-number field becomes a 0–15 alpha value. This ignores `REG_BLDY`, `REG_BLDALPHA`, *and* the mode in `REG_BLDCNT` entirely.

All effects can be confined to a screen region using windows.

### Mosaic

Block sizes 1×1 to 16×16. Backgrounds have a global enable; **sprites must opt in individually** (a flag in `oamSet()`) though the size setting is global. Documented game use: increasing mosaic over a few frames then deleting the sprite, to simulate a digital disintegration effect.

---

## 4. 3D from the programmer's side

### The polygon limit, confirmed a third time

> "There is a limit of 6144 vertices per frame. You can draw up to 2048 triangles or 1536 quads."

This matches the `RAM_COUNT` register exactly and independently confirms the correction in the main document. **1,536 quads, not Copetti's 1,706.** Anything sent after the limit is **silently ignored**.

### Fixed-point formats

No FPU, so everything is fixed-point, and the formats differ per purpose:

| Type | Format | Used for |
|---|---|---|
| `fixed12d3` | 12.3 | depth distances |
| `t16` | 12.4 | texture coordinates |
| `v16` | 4.12 | vertices (range −8.0 to ~7.99976) |
| `v10` | 0.10 | normals, small vertices (−1.0 to ~0.99) |
| `f32` | 20.12 | matrix components |

The `v16` range is a real modelling constraint: **models must fit in roughly ±8 units**, so you scale down in your modelling tool and use `glScale()` at draw time.

### Vertex command variants — a bandwidth optimisation

There are five ways to submit a vertex, trading precision for FIFO writes:

- `GFX_VERTEX16` — full `v16` precision, **two writes** (XY, then Z)
- `GFX_VERTEX10` — `v10`, **one write**, reduced range
- `GFX_VERTEX_XY` / `XZ` / `YZ` — `v16`, two components; the third **inherits from the previous vertex**
- `GFX_VERTEX_DIFF` — three `v10` deltas added to the previous vertex (can overflow the internal `v16` — be careful)

This is why display lists are compact and why strips help: the format itself is designed around minimising FIFO traffic.

### Texture formats — the full list

Seven formats, six paletted:

| Format | Description |
|---|---|
| `GL_RGBA` | 16-bit, 5/5/5 + 1 alpha bit. No palette. 2 bytes/px. |
| `GL_RGB256` | 256-colour palette, 1 byte/px |
| `GL_RGB16` | 16-colour, 4 bits/px |
| `GL_RGB4` | 4-colour, 2 bits/px |
| `GL_RGB32_A3` | 32 colours + **3 bits alpha per pixel** (8 levels) |
| `GL_RGB8_A5` | 8 colours + **5 bits alpha per pixel** (32 levels) |
| `GL_COMPRESSED` | tex4x4 — 4×4 blocks, **3 bits/px** + variable palette |

The `A3`/`A5` formats are the interesting ones: **per-pixel alpha in a paletted format**, which is unusual for the era and much cheaper than 16-bit RGBA.

**tex4x4 is stored in three parts across specific banks:**

- Texel blocks → texture slot 0 or 2 (typically VRAM A or C)
- Palette indices → texture slot 1 (VRAM B)
- Palette data → palette VRAM

Texel data is always exactly double the size of the index data. BlocksDS's characterisation: "think of it as the JPEG format of the DS" — good for natural textures, bad for detailed ones. `grit` can't produce it; you need `ptexconv`.

Texture palettes go in banks **F (16 KB), G (16 KB), or E (64 KB)** — note the tutorial text says "F (64 KiB), G (16 KiB) and H (16 KiB)", which conflicts with GBATEK's bank sizes (F and G are 16 KB, E is 64 KB, H is 32 KB and is Sub-only). **Trust GBATEK here** — the tutorial appears to have a typo.

### Live texture editing — the 22-scanline window

This is the sharpest practical constraint in the whole set:

> "You don't even have the full vertical blanking period to do the modifications. The GPU starts to render lines 48 scanlines before the end of the vertical blanking period. Vertical blanking starts in scanline 192, and 3D rendering starts in scanline 214. **In practice, you only have 22 scanlines to upload data to VRAM.** If you don't make it in time, the GPU will read white pixels from VRAM."

So the sequence for palette-cycling or streamed textures is: VBL handler → get pointer → set bank to LCD → edit → set bank back — all inside **22 scanlines**. Miss it and you get white.

**While textures or palettes are being copied, the GPU reads white pixels.** That's the visible failure signature.

### Translucency ordering — where polygon IDs earn their keep

Opaque polygons sort themselves. Translucent ones don't:

- Translucent polygons of the same object that don't blend with each other can share a polygon ID.
- Polygons meant to overlap *other* translucent polygons need **different IDs** — a translucent pixel is discarded if its ID matches what's already there.
- You must draw far-to-near yourself and finish with `glFlush(GL_TRANS_MANUALSORT)`. Otherwise the hardware Y-sorts, "which probably isn't what you want."

**The double-draw trick for translucent closed objects:** render each object twice — once with front culling (back faces), once with back culling (front faces), with different polygon IDs — and do this per object before moving to the next. You can't batch all objects' back faces then all front faces.

**Alpha value 0 means wireframe, not invisible.** To hide a polygon, don't draw it. (Wireframe works by filling only the edges, which ties back to the edge-flag machinery in the melonDS document.)

### Lighting

Four directional lights maximum, no positional lights. Four material properties: **diffuse, ambient, specular, emission**. Ambient only applies if lights are active; emission applies regardless.

Specular has an optional **128-entry shininess table**; disabled it behaves linearly.

**Point lights are faked** by recomputing a directional light's direction and colour per object, attenuating by `1/(k·d²)`. Convincing except when the source is very close to the object.

Two notes with teeth:

- **Normal and colour commands overwrite each other.** You cannot use both on the same vertex.
- Per a melonDS forum thread, the fastest submission order is **normal → texture coordinate → vertex**, not texcoord → normal → vertex.

### Toon vs decal vs modulation

`POLY_MODULATION` (default) behaves conventionally. `POLY_DECAL` is genuinely different: translucent polygons and translucent texture areas act like **white textures affected by light**, and lights do *not* tint the texture colour.

Toon shading uses the light calculation output as a table index — e.g. two ranges (0–15 dark, 16–31 lit) gives hard cel shading. Selected via `POLY_TOON_HIGHLIGHT` in `glPolyFmt()`, with the global toon/highlight choice in `DISP3DCNT.1` as covered in the GBATEK addendum.

### Antialiasing and edge marking

- AA doesn't affect translucent pixels.
- Line and point polygons, and wireframe polygons, get softened — **often to the point of being too faint to see**.
- **Making the clear plane transparent disables AA between polygons and the clear plane** — an unavoidable cost of compositing 3D over 2D.
- Edge marking supports **8 colours**, selected by polygon ID ÷ 8 (IDs 0–7 → colour 0, 8–15 → colour 1, …).
- Edges only appear **between polygons with different IDs**, so a single-ID sphere gets an outline against its surroundings but no internal edges.
- **The clear plane has a polygon ID too** — if a polygon's ID differs from it, you get an outline around the screen border. This is the same artifact GBATEK notes for clipping.
- Enabling both AA and edge marking on the same pixels gives edge marking at 50% opacity, and BlocksDS's assessment of the combination is "which isn't very good."

---

## 5. Techniques worth knowing exist

### Picking (touch → 3D object)

Render the scene **twice**: first with the viewport set off-screen (`glViewport(0, 192, 0, 192)`) and a `gluPickMatrix()` centred on the touch point defining a small (e.g. 4×4 px) window. Track `GFX_POLYGON_RAM_USAGE` before and after each object — if the count increased, that object is inside the pick window. Then render normally.

The picking pass can use simplified, untextured models. Combine with the **position test** (which returns transformed X/Y/Z/W for a single vertex) to disambiguate by depth when several objects are in the window.

**Not all emulators implement `GFX_POLYGON_RAM_USAGE`. melonDS does.** Useful compatibility datapoint.

### Volumetric shadows

The stencil mechanism, concretely:

1. Enable blending and manual translucent sorting.
2. Draw the scene.
3. Draw the shadow volume with `POLY_SHADOW`, **polygon ID 0**, alpha 1–30 → sets the stencil mask, draws nothing.
4. Draw the shadow volume *again* with `POLY_SHADOW`, your desired alpha, and a **non-zero polygon ID** → renders the shadow.
5. **Repeat per shadow.** Batching all masks then all volumes breaks shadow-on-shadow.

Inverting the brightness gives a flashlight effect instead.

### Skeletal animation via the matrix stack

The DS can do hardware skinning, with two hard limits:

- **Maximum 30 bones** — that's all the matrix stack holds.
- **One bone per vertex, weight 1.0 only.** Blended weights require per-combination matrices computed on the CPU, which gets expensive fast.

Vertices are stored relative to their bone's origin; the display list uses `MATRIX_RESTORE` to switch matrices mid-draw. Notably, **you can change the active matrix in the middle of a polygon** — vertices of one polygon need not share a matrix. (This is exactly the behaviour Shonumi found *Cory in the House* relying on, per the GBATEK addendum §13a.)

The DSMA library implements this, storing animation as quaternion + translation per bone (7 values vs 12 for a matrix) because **quaternions interpolate linearly and matrices don't**.

### Clear bitmap (and why nobody used it)

You can replace the solid clear colour with a bitmap: image in texture slot 2 (VRAM C), depth map in texture slot 3 (VRAM D), enabled via `GL_CLEAR_BMP`. Lock both banks so the texture allocator doesn't overwrite them.

BlocksDS's verdict is that it's not worth it: the image must be 16-bit (large, inflexible), a 2D background layer behind the 3D output is more flexible and can use other banks, and **the depth map is nearly impossible to author** — depth values depend on your near/far planes and on whether you're in Z or W mode, so it's trial and error.

Useful mainly as an explanation for why you'll rarely see it in a real game.

### W vs Z buffering, practically

- **Z**: depth distributed uniformly near-to-far. Better when geometry is spread evenly.
- **W**: more accurate near the camera, less accurate far away. Usually better for normal 3D scenes.
- **W is "not recommended for orthogonal projections"** — matching GBATEK's note that W-buffering "does not function properly with orthogonal projections."

Depth range is 0 (near) to `0x7FFF` (far). Z-fighting is easy to hit given fixed-point maths, and the mode choice is a real mitigation.

---

## 6. Compositing 2D and 3D — the practical rules

Two rules govern everything:

1. **3D output replaces BG0 of the main engine.** (Confirmed against GBATEK.)
2. **3D output has its own alpha channel.** To see 2D beneath it, the clear plane must be transparent: `glClearColor(0, 0, 0, 0)`.

**2D over 3D:** lower BG0's priority, raise the layer you want on top. Sprites are above backgrounds by default. Nothing more needed.

**3D over 2D:** transparent clear plane — at the cost of AA against the clear plane, as noted above.

**Alpha fog over 2D** is the fiddliest combination: enable `GL_FOG_ONLY_ALPHA`, transparent clear plane, and set up the 2D blend registers with destination layers — the 3D layer is set as source automatically and **EVA/EVB are ignored**. This is the third blending path from GBATEK §9 ("blending with BG0 as 1st target via 3D alpha values, *unlike* for 2D"), seen from the programmer's side. GBATEK marked its own description of the mechanism as uncertain; BlocksDS confirms the EVA/EVB values are ignored in practice.

The stated use case is genuinely nice: 3D objects fading into a 2D background as they recede.

### Why a 2D background beats 3D geometry for backdrops

> "Drawing a tiled background using the 3D hardware needs hundreds of polygons, which takes a lot of CPU time and GPU resources. A 2D tiled background is basically free for the CPU and GPU."

Which is the whole design thesis of the console restated from the trenches: use each engine for what it's cheap at, and composite. The 3D engine buys you unlimited transformed sprites; the 2D engine buys you free static backdrops. Games that look best on the DS use both.

---

## 7. The CPU cost nobody mentions in hardware docs

> "A downside of using the 3D engine is that you need to tell the GPU what to draw **every frame**. With the 2D hardware you don't have to do anything once you've configured your background and sprites."

This is the fundamental asymmetry between the two engines, and it's invisible in a register reference. 2D is **declarative** — configure once, the hardware redraws it forever. 3D is **immediate mode** — resubmit the whole scene every frame, burning CPU on FIFO writes.

Display lists are the mitigation: pre-packed command arrays (4 commands per 32-bit word, `FIFO_NOP`-padded) sent by DMA, so complex static models cost almost no CPU. The first word is the length in words, excluding itself.

And GBATEK's note that the hardware **re-renders the same image automatically** if the geometry engine doesn't supply new data is the flip side: a static 3D scene doesn't require resubmission, it just repeats.

---

## 8. Reading order for the whole set

1. **`nintendo-ds-graphics-rundown.md`** — the conceptual model: screens, layers, engines, what 3D is for.
2. **This document** — what it costs to actually use, and the techniques built on it.
3. **`gbatek-ds-video-addendum.md`** — the register-level ground truth; the reference you return to.
4. **`melonds-rendering-internals.md`** — what breaks when you reimplement it, and why the quirks exist.

If you only read two, make it 1 and 3.
