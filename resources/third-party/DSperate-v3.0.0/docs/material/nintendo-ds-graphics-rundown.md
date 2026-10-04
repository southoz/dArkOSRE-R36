# Nintendo DS Graphics: Screens, Layers, and 3D

A reference assembled from two sources:

- **Video transcript** — overview of the DS 2D/3D engines (source of the *New Super Mario Bros.* layer breakdown and the dual-screen 3D trick).
- **Rodrigo Copetti, "Nintendo DS Architecture — A Practical Analysis"** — <https://www.copetti.org/writings/consoles/nintendo-ds/> (CC BY 4.0). More precise on register-level details; used as the tiebreaker where the two disagree.

Conflicts between the two are called out in **[Discrepancy]** notes rather than smoothed over.

---

## 1. The physical display

| Property | Value |
|---|---|
| Screens | 2 × LCD |
| Resolution | 256 × 192 each |
| Colour depth | Panels are 18-bit (262,144 colours), but **the 2D engines work in 15-bit BGR** (32,768 colours) — same colour format as the GBA. Only the 3D layer reaches 18-bit. See addendum §2. |
| Refresh | ~60 Hz |
| Bottom screen | Resistive touch panel, stylus-driven, controlled over SPI by the ARM7 |

For reference: GBA was 240 × 160, so the DS gives roughly 20% more pixels *per screen*. Both screens together is a substantially different rendering budget than a single GBA panel — worth remembering when reasoning about why the polygon ceiling is where it is.

An undocumented touchscreen quirk: the controller can return a diagonal position, from which a pressure/contact-area value can be derived. It was never officially exposed and no retail game used it. (Copetti specifically debunks the widely repeated claim that *Hotel Dusk*'s two-finger switchboard puzzle uses pressure — debugger tracing shows it just watches for X/Y values alternating sharply.)

---

## 2. Engine topology

This is the single most important structural fact, and everything downstream follows from it:

```
        ┌─────────────┐            ┌─────────────┐
        │  2D "Main"  │            │  2D "Sub"   │
        └──────┬──────┘            └──────┬──────┘
               │                          │
        ┌──────┴──────┐                   │
        │  3D engine  │  (Main only)      │
        └─────────────┘                   │
               │                          │
          Screen A                   Screen B     (assignment is swappable)
```

- **Two 2D engines**, named **Main** and **Sub**. Main is strictly more capable.
- **One 3D engine**, hard-wired to Main.
- Each engine drives exactly one screen, but *which* screen is a software choice. Assign Main to the top and Sub falls to the bottom, and vice versa.
- Consequence: **3D is only available on one screen at a time**, and only on whichever screen Main is currently driving.

### The dual-screen 3D trick

**[Corrected — the video's explanation is wrong.]** The transcript says you flip Main/Sub every frame and "construct and render 3D graphics on both screens." That's not the mechanism, because there is still only one 3D engine and it can't render two scenes in one frame.

The actual technique, per Shonumi (GBE+ author), works through **display capture**:

1. Engine A renders a 3D scene and **captures** it to VRAM.
2. That VRAM is passed to Engine B, which displays the captured image as a plain 2D bitmap layer.
3. Meanwhile Engine A is swapped to the other screen and renders a *different* scene.
4. Repeat, alternating every frame.

So Engine B acts as Engine A's **"shadow,"** displaying the *last* 3D image generated rather than a live one. The consequence: **dual-screen 3D is limited to 30 FPS**, since each screen only gets a freshly rendered frame every other frame.

This works reliably on hardware and is how commercial games did it. It also explains why the effect is hard to emulate at upscaled resolutions — see the capture-depth constraint in the GBATEK addendum §6.

---

## 3. VRAM

**656 KB total**, and critically **not one contiguous block**. It is split into nine banks:

| Count | Size |
|---|---|
| 4 | 128 KB |
| 1 | 64 KB |
| 1 | 32 KB |
| 3 | 16 KB |

Banks are individually mapped by the programmer to a purpose — background data (tilesets, tilemaps, or bitmaps), sprite/object data, 3D textures, palettes, or as extended work RAM.

Allocation is not free-for-all. The constraint set includes:

- Both 2D engines can read any bank, but **not the same bank concurrently**.
- The ARM7 can only reach two of the 128 KB banks.
- Those same two banks **cannot hold sprite data**.
- The last 16 KB bank is **Sub-only**.
- The 3D engine draws its textures from a subset of banks (up to ~512 KB addressable for texture data).

For comparison, the GBA had 96 KB. The jump is large, but bank-mapping constraints mean the usable figure for any given configuration is well below 656 KB.

---

## 4. Building a 2D frame

Every mode produces **four background layers** plus a sprite/object layer. What differs per mode is the *type* of each of those four layers.

### 4.1 Tiles

8 × 8 pixels, unchanged in principle since the Game Boy. Per-tile features:

- Horizontal and vertical flipping
- Palette selection from 16 palettes

**[Discrepancy]** The transcript says each tile holds 16 colours and picks one of 16 palettes "for a total of 256 colours." Copetti describes a static background as using **256 colours and 16 palettes**. These describe different things — 4bpp tiles (16 colours × 16 palettes) is the classic GBA-style arrangement the video is describing; the DS also supports 8bpp tiles with extended palettes. Treat the video's phrasing as the 4bpp case, not the whole story.

The transcript's point about flipping being a *space* optimisation is worth internalising: a symmetric image can be built from a handful of unique tiles plus flip bits, which matters far more on a cartridge-budget system than the feature sounds like it should.

### 4.2 Background types

Copetti's taxonomy is cleaner than the video's and is the one to memorise:

**Tile-based group**

| Type | Size | Tiles | Notes |
|---|---|---|---|
| **Static** (text) | up to 512 × 512 | 1,024 | H/V flip, scrolling, mosaic, alpha blend, plus a fade effect |
| **Affine** | 1024 × 1024 | 256 | Affine transforms, but **no H/V flip** |
| **Affine Extended** | — | 1,024 | Affine transforms **and** H/V flip — strictly better than Affine |

**Bitmap group** (VRAM treated as a framebuffer, plot pixels directly)

| Type | Size | Colours |
|---|---|---|
| Affine Extended, 256-colour | 512 × 512 | 256 (paletted) |
| Affine Extended, direct colour | 512 × 512 | 32,768 (15-bit) |
| **Large screen** | 1024 × 512 | consumes a full 128 KB bank |

**3D background**

- Displays the 3D engine's output as a background layer.
- Supports horizontal scrolling and alpha blending against other BG layers.
- **The only layer type that reaches the full 18-bit / 262,144 colours.**

Affine transformations cover scaling, rotation, shearing, and reflection — the same family of tricks as SNES Mode 7 and the GBA's affine layers, applied per-layer in real time.

### 4.3 Background modes

**[Discrepancy]** The video's mode list is loose and partly wrong (it describes modes 3–5 as each "adding an extended affine layer" and gives mode 2 as "two text and two affine"). Copetti's list is register-accurate:

| Mode | Composition |
|---|---|
| 0 | 4 × Static |
| 1 | 3 × Static + 1 × Affine |
| 2 | 2 × Static + 2 × Affine |
| 3 | 3 × Static + 1 × Affine Extended |
| 4 | 2 × Static + 1 × Affine + 1 × Affine Extended |
| 5 | 2 × Static + 2 × Affine Extended |
| 6 | 1 × 3D background + 1 × Large screen |

Two rules layered on top:

1. **In modes 0–5, the Main engine may convert its first static layer (BG0) into the 3D background layer.** This is *why* every mode is guaranteed at least one static layer — it's the socket the 3D output plugs into.
2. **Mode 6 is Main-only**, because there is only enough VRAM headroom for a single large framebuffer.

**Mode 5 is the workhorse.** Both sources agree here, and the reasoning is clear: you get 3D on BG0, two Affine Extended layers (which can each be tiled *or* bitmap, your choice), and a static layer — maximum flexibility with no capability sacrificed.

Note the "Affine Extended" slots are a *family*: whenever a mode grants one, you pick the flavour (tiled, 256-colour bitmap, or direct-colour bitmap) at configuration time. This is where the video's "mix bitmaps with tiles for interesting effects" comes from.

### 4.4 Scrolling

Hardware scroll registers, two per background layer (X and Y) × four layers = eight registers per engine. Per-scanline writes to these registers are how you get parallax and wobble effects — see the *NSMB* cloud layer below.

### 4.5 Sprites (Objects)

Inherits the GBA's OAM model, with two meaningful upgrades:

- **OAM is 2 KB**, split 1 KB per engine → **128 sprites per screen per frame**.
- Sprites may reference **bitmaps directly from VRAM** instead of tiles+palette. This is set *per sprite*, so tile-based and bitmap-based sprites coexist in the same frame freely.

Size range is unchanged from GBA: 8 × 8 minimum up to 64 × 64.

### 4.6 The technique none of the primary sources mention: 3D-as-2D

Per Shonumi, a **very common** pattern in commercial DS titles is to ignore the 2D engine for sprites entirely and instead use the **3D engine with orthogonal projection**, drawing "flat" textured quads.

Why developers did this:

- It offers **far more "sprites"** than the 128-per-engine OAM limit allows.
- It's essentially how modern 2D rendering works — textured quads on a GPU.
- You get free rotation, scaling, alpha blending, and per-vertex colour tinting on every sprite, without consuming affine parameter slots.

The cost is your polygon and vertex budget, plus the fact that it occupies BG0 and locks you out of a second 3D scene.

This matters for the mental model: **a DS game showing no visible 3D may still be using the 3D engine for nearly everything on screen.** Shonumi notes that implementing texture formats in GBE+ "opened a whole new world" precisely because so many 2D-looking games are 3D underneath — *Puyo Puyo*, *Castlevania*, *Pokémon Trozei*, and *A Witch's Tale* are cited examples.

---

## 5. Worked example: *New Super Mario Bros.*

The transcript's layer dissection, which is a good mental model for how a real DS game distributes work:

| Layer | Content | Technique |
|---|---|---|
| Sprites (OAM) | Coin counter, timer HUD | Standard objects |
| BG0 | Mario's model | **3D background** |
| BG1 (video: "BG2") | Horizon and clouds | Affine / per-scanline horizontal shift for parallax |
| BG2, BG3 | Level geometry, coins | Tile and bitmap based |

**[Discrepancy]** The video numbers the layers descending (level data on BG3/BG4, horizon on BG2, Mario on BG1); Copetti's screenshots label them BG0/BG2/BG3 with BG0 carrying the shifted cloud layer. The *structure* is identical in both accounts — a 3D character composited into a 2D tile world with a parallax sky — but don't trust either source's specific layer indices without checking the ROM.

The headline point stands: Mario himself is a 3D model living in BG0's 3D slot, while the world he runs through is conventional 2D tiles. That hybrid is the DS's signature move.

---

## 6. The 3D engine

Two discrete components inside CPU NTR, conceptually similar to the N64's Reality Coprocessor split.

### 6.1 Geometry Engine

Handles:

- Vertex transformations
- Projection
- Lighting
- Clipping
- Culling
- **Polygon sorting** (required for transparency to work correctly)

This exists because the ARM9 is poor at vector maths and the dedicated divider/sqrt unit isn't sufficient — there is no SIMD unit anywhere in the system.

**Command submission:** a **Command FIFO** holding 256 entries, plus a small **PIPE** buffer holding 4 more (260 total). Filled by the CPU or by DMA.

**The polygon ceiling.** 248 KB of dedicated RAM stores processed geometry, which works out to:

- **2,048 triangles**, or
- **1,706 quadrilaterals**

**[Settled — GBATEK `RAM_COUNT` register, 4000604h]** The register that reports current usage defines the limits unambiguously:

- Bits 0–11: polygons currently in Polygon List RAM, **0..2048**
- Bits 16–28: vertices currently in Vertex RAM, **0..6144**

So it is **2,048 polygons and 6,144 vertices**, both hard, whichever binds first. Copetti's "1,706 quadrilaterals" figure is not supported by the hardware registers and should be discarded; the quad limit is min(2048, 6144/4) = **1,536** if you use no strips. Arisotura's framing was right.

Strips reduce vertex consumption only. The polygon count is a hard wall because the engine must **store every polygon in order to Y-sort it**.

Throughput figure worth having: **~122,880 polygons/second** under optimal conditions.

Also note the DS renders quads natively — it is not a triangle-only pipeline, which is why the quad figure is a separate number rather than a division by two.

### 6.2 Rendering Engine

Rasterisation, texturing, lighting, effects. Feature list:

- **Perspective-correct** texture interpolation
- **Gouraud shading** (the video says "grad shading" — mis-transcription)
- **Depth buffering** — Z-buffering *or* a W-buffering variant
- **Alpha blending**
- **Stencil tests**
- **Fog**
- **Anti-aliasing** — very primitive: it simply makes outer polygon edges transparent, and works on opaque pixels only
- **Shadowing** in hardware
- **Toon shading** — cel shading. Not programmable; you adjust lighting parameters to get the cartoon banding. Both sources note this was a fashionable look at the time and getting it for free in hardware was genuinely useful.

### 6.3 Line-buffer rendering — the important architectural detail

The 3D engine **does not output a framebuffer.** It uses **line buffer rendering**, filling scanlines and storing results in a small **Colour Buffer holding 48 scanlines**. The 2D engine pulls scanlines from this buffer FIFO-style to populate BG0.

Why: the 3D engine has to run *in lockstep with the 2D scanline drawer*. It's a raster-synchronous design, not a deferred one.

Consequences worth knowing:

- 3D rendering starts *before* 2D for the frame, so the 2D engine can then apply layer transformations to the 3D output.
- The rasteriser traverses each scanline looking for polygon edges. Per Arisotura (melonDS), **for each quadrangle only one span per scanline can be filled** — so concave quads or quads with crossed edges render incorrectly. This is a real, exploitable-in-emulation quirk.
- Rendering parameters can be changed mid-frame thanks to double-buffering of engine state; the previous state is preserved until the current frame finishes, so no tearing.

### 6.4 Capture

Main can **capture** the generated 2D, 3D, or combined frame, blend it with another frame already in VRAM, and write the result back to VRAM for later display. This is the mechanism behind motion-blur and feedback effects in DS games.

---

## 7. Why DS 3D looks the way it does

The N64 comparison both sources reach for, with Copetti's explanations for each observed artefact:

| Observation | Cause |
|---|---|
| Textures look **blocky** | No filtering at all — nearest-neighbour only. No bilinear. |
| Textures look **richer** than N64 | N64 was choked by a 4 KB texture cache. The DS has up to ~512 KB of VRAM for textures plus compression formats. More texture *data*, worse texture *filtering*. |
| Models have **pixelated edges** | Lower render resolution (256 × 192 vs N64's 320 × 240) plus weak AA. |
| Textures **warp at distance** | Fixed-point rasteriser coordinates, no mip-mapping, low resolution → aliasing. |

So the DS is simultaneously *behind* the N64 (no bilinear, no mipmapping, no subpixel precision) and *ahead* of it (far more texture memory, hardware toon shading, stencil, fog, better colour depth on the 3D layer). "Portable N64" is the wrong model; it's a differently-shaped machine.

Model budgets in practice, from Copetti's teardowns:

- *Nintendogs* (2005) — **750 triangles**
- *New Super Mario Bros.* (2006) — **636 triangles**

Against a 2,048 ceiling, that's a lot of headroom left for environment geometry — and it's why DS games rarely dropped frames. The low ceiling forced conservative budgets, which incidentally bought consistent performance.

---

## 8. The design thesis

Both sources converge on the same reading, and it's the thing worth taking away:

The DS is not a technology bet. It reuses a mature, well-understood 2D tile engine — lineal descendant of the SNES and GBA PPUs — and bolts on a *modest* 3D unit whose job is largely to supply one background layer to that 2D compositor. The interesting output comes from the **composition**: 3D models depth-buffered against 2D layers, bitmap and tile sprites in the same frame, affine layers transforming 3D output.

The PSP won the raw-polygon contest outright. The DS won by making 2D and 3D interoperate cleanly on hardware cheap enough to sell 150M+ units with usable battery life.

---

## 9. Open threads worth chasing

- **8bpp tiles and extended palettes** — neither source covers the DS's extended palette modes in depth; GBATEK is the place to go.
- **Texture compression formats** — Copetti mentions "plenty compression mechanisms" without enumerating them. The DS's 4x4-block compressed texture format is genuinely unusual and worth a separate read.
- **Exact VRAM bank mapping table** — the constraint list here is a summary; the full mapping matrix (which bank can serve which purpose under which control register value) is in GBATEK's `DS Video VRAM` section.
- **Martin Korth's GBATEK** — <https://problemkaputt.de/gbatek.htm> is the primary source both the article and the video are ultimately downstream of.
- **Arisotura's melonDS blog** — the "DS GPU and its fun quirks" post is where the one-span-per-scanline behaviour and similar edge cases are documented.
