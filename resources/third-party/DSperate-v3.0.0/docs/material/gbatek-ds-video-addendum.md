# GBATEK Addendum — Register-Level DS Video

Third document in the set. Source: **GBATEK** by Martin Korth, extracted from no$gba, read directly from the plaintext dump — sections `DS Video`, `DS Video Stuff`, `DS Video BG Modes / Control`, `DS Video OBJs`, `DS Video Extended Palettes`, `DS Video Capture and Main Memory Display Mode`, `DS Video Display System Block Diagram`, `DS Memory Control - VRAM`, `DS 3D Overview`, `DS 3D Display Control`, `DS 3D Status`, `DS 3D Final 2D Output`.

This is the authoritative source. Where it disagrees with the other two documents, it wins.

---

## 0. Verdicts on earlier open questions

| Question | Verdict |
|---|---|
| **Background mode table** | **Copetti's table is correct.** GBATEK's table matches it exactly. The mgBA-GBATEK AI mirror's table was wrong — disregard it. |
| **Polygon limit** | **2,048 polygons / 6,144 vertices**, both hard caps, per `RAM_COUNT`. Copetti's "1,706 quads" is unsupported. Arisotura was right. |
| **Colour depth** | 2D is **15-bit**. The 18-bit figure applies only to 3D. Confirmed by the capture unit spec (see §6). |
| **Dot clock** | **5.585664 MHz** (33.513982 / 6). The mgBA mirror's "5.96 MHz / divide by 5.625" is wrong. |
| **Mode 6 layer count** | Only **two** layers: BG0 = 3D (mandatory), BG2 = Large bitmap. Not "1 3D + 1 large screen among four." |

---

## 1. Exact timings

```
Dot clock  5.585664 MHz  (= 33.513982 MHz / 6)
H: 256 visible + 99 blanking = 355 dots   (15.7343 kHz)
V: 192 visible + 71 blanking = 263 lines  (59.8261 Hz)
Screen     62.5mm x 47.0mm each
Gap        22mm vertical (≈90 pixels equivalent)
```

**3D V-Blank is lines 191–213** — 23 lines, and *not* the same window as the 2D V-Blank. Worth knowing if you're timing GX command submission.

Two timing gotchas:

- The V-Blank flag isn't set on the **last** line: it's set for lines 192–261, but **not 262**. Same as GBA.
- Drawing time is 1,536 cycles (256×6), but the NDS9 H-Blank flag reads `0` for **1,606** cycles — and **1,613** on NDS7, for unexplained reasons.

`VCOUNT` is **writable**, which exists specifically to let linked consoles synchronise. GBATEK gives a protocol: only write new LY values in range 202–212, and only while the old LY is in 202–212.

### VRAM waitstates

The display controller reads VRAM **once every 6 clock cycles**; a simultaneous CPU access costs a **1-cycle waitstate**. With **capture enabled**, VRAM writes also occur every 6 cycles, so the combined read/write access rate becomes **once every 3 cycles** — doubling contention. Relevant if you're profiling a game that uses capture.

---

## 2. Per-engine memory limits (the asymmetry, concretely)

| Region | Engine A | Engine B |
|---|---|---|
| I/O ports | 4000000h | 4001000h |
| Palette | 5000000h (1K) | 5000400h (1K) |
| BG VRAM | 6000000h, **max 512K** | 6200000h, **max 128K** |
| OBJ VRAM | 6400000h, **max 256K** | 6600000h, **max 128K** |
| OAM | 7000000h (1K) | 7000400h (1K) |

Engine A additionally has: 3D, large-screen 256-colour bitmaps, VRAM display mode, main-memory display mode, and the capture unit. Engine B has none of these.

The BG VRAM asymmetry is **4:1**, which is a much bigger deal than "Main is a bit more capable" suggests. Bitmaps requiring more than 128K are Engine A only.

---

## 3. DISPCNT — the register that drives everything

`4000000h` (Engine A) / `4001000h` (Engine B):

| Bits | Engine | Meaning |
|---|---|---|
| 0–2 | A+B | BG Mode |
| **3** | **A** | **BG0 2D/3D selection (0=2D, 1=3D)** — occupies the GBA's CGB Mode bit |
| 4 | A+B | Tile OBJ mapping (0=2D max 32K, 1=1D max 32K–256K) |
| 5 | A+B | Bitmap OBJ 2D dimension (0=128×512, 1=256×256) |
| 6 | A+B | Bitmap OBJ mapping (0=2D max 128K, 1=1D max 128K–256K) |
| 16–17 | A+B | **Display Mode** (A: 0–3, B: 0–1) |
| 18–19 | A | VRAM block (0–3 = VRAM A–D) for capture and display mode 2 |
| 20–21 | A+B | Tile OBJ 1D boundary |
| 22 | A | Bitmap OBJ 1D boundary |
| 23 | A+B | OBJ processing during H-Blank (was bit 5 on GBA) |
| 24–26 | **A only** | Character base, 64K steps (merged with BGxCNT's 16K step) |
| 27–29 | **A only** | Screen base, 64K steps (merged with BGxCNT's 2K step) |
| 30 | A+B | BG extended palettes enable |
| 31 | A+B | OBJ extended palettes enable |

**Bits 24–29 are Engine A only.** Engine B's char/screen base has no 64K component — its bases are `BGxCNT.bits*16K + 0` and `BGxCNT.bits*2K + 0`. Another concrete piece of the asymmetry.

### Display Mode (bits 16–17)

| Value | Behaviour |
|---|---|
| 0 | Display off — **screen becomes white** |
| 1 | Graphics display (normal BG + OBJ) |
| 2 | **A only:** VRAM display — raw bitmap from the block in DISPCNT.18-19 |
| 3 | **A only:** Main Memory display — bitmap DMA'd from main RAM |

Modes 2 and 3 show a raw 15-bit direct-colour bitmap and **completely bypass both the 2D and 3D engines** plus all 2D effects — but **Master Brightness still applies**. Mode 2 is the intended way to display captured images.

None of the other sources mention modes 2 and 3 at all, and they matter: they're a full framebuffer path that sidesteps the tile engine entirely.

---

## 4. Background modes — GBATEK's authoritative table

```
Mode  BG0      BG1      BG2       BG3
0     Text/3D  Text     Text      Text
1     Text/3D  Text     Text      Affine
2     Text/3D  Text     Affine    Affine
3     Text/3D  Text     Text      Extended
4     Text/3D  Text     Affine    Extended
5     Text/3D  Text     Extended  Extended
6     3D       -        Large     -
```

Engine B: identical **except** mode 6 is reserved (no large bitmap) and **BG0 is always Text** (no 3D).

Note `Text/3D` on BG0 in modes 0–5 — the 3D layer is *always* BG0, never any other slot, and it's selected by DISPCNT bit 3.

### Extended affine sub-selection

"Extended" isn't one thing — it's three, chosen by two bits in `BGxCNT`:

| BGxCNT.7 | BGxCNT.2 | Result |
|---|---|---|
| 0 | (CharBase LSB) | **rot/scal with 16-bit BG map entries** — a Text+Affine hybrid |
| 1 | 0 | rot/scal 256-colour bitmap |
| 1 | 1 | rot/scal direct-colour bitmap |

That first row is a mode none of the other sources describe: **affine transformation with 16-bit tilemap entries**, meaning you get per-tile flipping and palette selection *and* rotation/scaling simultaneously. On the GBA these were mutually exclusive. This is arguably the single most useful 2D upgrade the DS made, and it's the reason "Affine Extended" beats plain "Affine."

The bit-stealing is elegant: extended affine has no 16-colour mode, so the colour-depth bit is free for mode selection; and bitmap modes don't use charbase, so charbase bit 0 is free too.

### Screen sizes by type

| BGxCNT size | Text | Rot/Scal | Bitmap | Large bitmap |
|---|---|---|---|---|
| 0 | 256×256 | 128×128 | 128×128 | **512×1024** |
| 1 | 512×256 | 256×256 | 256×256 | **1024×512** |
| 2 | 256×512 | 512×512 | 512×256 | — |
| 3 | 512×512 | **1024×1024** | 512×512 | — |

Large bitmap goes to **512×1024 or 1024×512** — larger than Copetti's "1024×512" alone, and it uses all 512K of Engine A's 2D VRAM. Note that large-bitmap mode uses **no screen base at all**.

### Direct-colour alpha

Direct-colour BG and OBJ use 15-bit RGB, but **bit 15 is an alpha flag**: 0 = transparent, 1 = normal. So direct-colour values `0000h..7FFFh` are **not displayed**. This trips people up — it's not the GBA's behaviour, where all 16 bits were colour.

Also: unlike GBA, NDS bitmap modes **do** support the Area Overflow bit (BG2CNT/BG3CNT bit 13).

---

## 5. Extended palettes — the detail Copetti skips entirely

Enabled per-engine by DISPCNT bits 30 (BG) and 31 (OBJ).

**BG extended palettes:** allocated VRAM splits into **4 slots of 8K each (32K total)**. Normally BG0–3 use slots 0–3, but BG0 and BG1 can be redirected via `BG0CNT.13` / `BG1CNT.13` to slots 2 and 3 respectively.

When enabled, the split is:

- **Standard palette** still serves: 16-colour tiles (16-bit bgmap entries), 256-colour tiles (8-bit bgmap entries), 256-colour bitmaps, and the backdrop colour.
- **Extended palette** serves: **256-colour tiles with 16-bit bgmap entries** (both text and rot/scal).

**OBJ extended palettes:** takes you from 16 colours × 16 palettes (256 total, standard memory) to **256 colours × 16 palettes = 4,096 colours**. Must live in VRAM F, G, or I, of which only the **lower 8K** of the 16K bank is used.

**The catch:** allocated extended-palette memory is **not mapped to the CPU bus**. To write to it you must temporarily de-allocate it. Same restriction applies to texture image and texture palette VRAM.

This is the mechanism behind "DS 2D looks better than GBA 2D" more than anything else in the spec sheet.

---

## 6. Display capture — full spec

`DISPCAPCNT` at `4000064h`, **Engine A only**:

| Bits | Field |
|---|---|
| 0–4 | EVA — blend factor for source A (0..16) |
| 8–12 | EVB — blend factor for source B (0..16) |
| 16–17 | VRAM write block (0–3 = A–D); **must be allocated to LCDC** |
| 18–19 | VRAM write offset (0/8000h/10000h/18000h) |
| 20–21 | Capture size (0=128×128, 1=256×64, 2=256×128, 3=256×192) |
| 24 | Source A (0 = graphics screen BG+3D+OBJ, 1 = **3D screen only**) |
| 25 | Source B (0 = VRAM, 1 = main memory display FIFO) |
| 26–27 | VRAM read offset |
| 29–30 | Capture source (0=A, 1=B, 2/3=A+B blended) |
| 31 | Capture enable / busy |

Blend maths when A+B selected:

```
Dest_Intensity = (SrcA_Int * SrcA_Alpha * EVA + SrcB_Int * SrcB_Alpha * EVB) / 16
Dest_Alpha     = (SrcA_Alpha AND EVA>0) OR (SrcB_Alpha AND EVB>0)
```

> **Capture data is 15-bit colour depth — even when capturing 18-bit 3D images.**

That single sentence is the root of melonDS's dual-screen-3D upscaling problem described in the companion document. Capture is lossy and fixed at native resolution and 15-bit by hardware definition, so an upscaled internal renderer has nowhere to put its extra data.

Capture starts at the next line 0 after enable, and the busy bit auto-clears **in line 192 regardless of capture size**. Sizes below 256×192 capture the upper-left portion.

The two documented effect recipes:

1. Capture 3D output via source A to LCDC VRAM; next frame, display it as **BG2, BG3, or OBJ** from BG/OBJ-allocated VRAM. Requires switching the bank between LCDC and BG/OBJ allocation. **This is how you get 3D onto a non-BG0 layer** — indirectly, one frame late.
2. Capture Engine A output, display it via VRAM display mode in following frames while capturing the new output blended with the old → **motion trails**. Works with a single LCDC-allocated block.

`DISP_MMEM_FIFO` at `4000068h` accepts 4 words (8 pixels) at a time via DMA — 32-bit width, word count 4, source in main memory. Transfer starts at the next frame.

---

## 7. VRAM banking — the real table

Nine banks: A–D at 128K, E at 64K, **F and G at 16K, H at 32K, I at 16K**. (Note H is 32K and sits between G and I — the "three 16K banks" are F, G, and I, not consecutive.)

`VRAMCNT_x` layout: bits 0–2 MST (bit 2 unused for A, B, H, I), bits 3–4 offset (unused for E, H, I), bit 7 enable.

Key mappings, condensed:

| Purpose | Eligible banks |
|---|---|
| LCDC (plain ARM9 access) | **All nine** |
| Engine A BG (max 512K) | A, B, C, D, E, F, G |
| Engine A OBJ (max 256K) | **A, B only** (of the 128K banks), E, F, G |
| Engine A BG ext. palette | E (lower 32K), F, G |
| Engine A OBJ ext. palette | F, G (lower 8K) |
| **Texture / rear-plane image** | **A, B, C, D only** — slot = OFS |
| Texture palette | E, F, G |
| Engine B BG (max 128K) | C, H, I |
| Engine B OBJ (max 128K) | D, I |
| Engine B BG ext. palette | **H only** |
| Engine B OBJ ext. palette | **I only** |
| ARM7 work RAM | **C, D only** |

This corrects a claim in the main document: **C and D cannot hold Engine A OBJ data** — only A and B can, among the large banks. The main document's "the two ARM7-accessible banks can't store sprites" was directionally right but for the wrong reason; it's a general restriction on C/D for Engine A OBJ, not a consequence of ARM7 mapping.

**Access rules:**

- In BG/OBJ modes, VRAM is CPU-accessible at the mapped address *and* readable by the display controller.
- In **extended palette and texture image/palette modes, VRAM is not mapped into CPU address space at all** — you must temporarily switch to plain-CPU (LCDC) mode to initialise it.
- All VRAM, palette, and OAM accept **16- and 32-bit writes only**. `STRB` is silently ignored. The sole exception is ARM7 plain-access mode, where `STRB` works — because GBA mode uses two 128K VRAM blocks to emulate the GBA's 256K work RAM.

---

## 8. 3D engine — corrections and additions

### Double-buffered geometry, and SWAP_BUFFERS

There are **two complete sets of Vertex/Polygon RAM** — one owned by the geometry engine, one by the rendering engine. `SWAP_BUFFERS` (cmd 50h, `4000540h`) exchanges them and empties the old one.

The command's two parameter bits are the actual home of two things the other documents describe as ambient properties:

- Bit 0: **translucent polygon Y-sorting** (0 = auto-sort, 1 = manual)
- Bit 1: **depth buffering** (0 = Z, 1 = W) — *"mode 1 does not function properly with orthogonal projections"*

Those bits apply to the commands **after** the swap, not before.

`SWAP_BUFFERS` doesn't execute until the next V-Blank (scanline 192), and the geometry engine is **halted** for that duration. Texture memory is **not** swapped, nor are the rendering control registers (4000060h, 4000330h–40003BFh) — software must keep both intact throughout rendering.

**Hard lockup quirk:** issuing `SWAP_BUFFERS` with an incomplete polygon list (e.g. a triangle with only two vertices) **locks the 3D hardware permanently**. 2D keeps working; any wait loop on `GXSTAT.27` hangs. There is no software recovery — not by sending the missing vertices, not even by pulsing `POWCNT1.2-3`.

### Rendering timing, precisely

Copetti says "3D rendering starts before 2D." GBATEK gives the numbers:

- Rendering **starts at scanline 214**, during V-Blank — i.e. **48 lines in advance**
- Output begins **after scanline 262**
- The 48-line cache is read out as the display consumes it while rendering continues writing

`RDLINES_COUNT` at `4000320h` reports the **minimum number of buffered lines (minus 2) in the previous frame**, 0..46. Falling values warn you the renderer is losing ground before glitches appear. If it hits zero you still don't know whether an actual underflow occurred — that's `DISP3DCNT.12`.

That's a genuine performance-tuning register, and it explains the design intent: the cache absorbs per-scanline overload, and the game is expected to monitor headroom and shed polygons.

GBATEK's rationale for having no framebuffer is worth quoting in paraphrase: animated data is normally drawn once per frame anyway, so storing it would be pointless. If the geometry engine *doesn't* supply new data, the hardware simply re-renders the same image every frame.

### DISP3DCNT (4000060h) — global 3D switches

| Bit | Function |
|---|---|
| 0 | Texture mapping enable |
| **1** | **Shading: 0 = Toon, 1 = Highlight** |
| 2 | Alpha test enable (vs `ALPHA_TEST_REF`) |
| 3 | Alpha blending enable |
| 4 | Anti-aliasing enable |
| 5 | Edge marking enable |
| 6 | Fog mode (0 = alpha and colour, 1 = alpha only) |
| 7 | Fog master enable |
| 8–11 | Fog depth shift |
| 12 | **Colour buffer RDLINES underflow flag** |
| 13 | **Polygon/Vertex RAM overflow flag** |
| 14 | Rear-plane mode (0 = blank, 1 = bitmap) |

**Toon and Highlight are mutually exclusive and global**, selected by one bit — not per-polygon. The melonDS document describes them as two uses of the same table, which is right, but they can't coexist in a frame.

Bits 12 and 13 are diagnostic flags games could actually poll.

### Alpha test

Enabled by DISP3DCNT.2, compares against `ALPHA_TEST_REF` (4000340h): pixels render only if alpha is **greater than** the reference. When disabled, the test is against zero. Applied to **final polygon pixels, after texture blending**. Ref value 0 is equivalent to disabled; **1Fh hides everything including opaque polygons**.

### 1-dot polygon culling — nobody else mentions this

`DISP_1DOT_DEPTH` at `4000610h`: polygons that would render as a single pixel and whose depth exceeds this threshold are **automatically hidden**, to save memory and reduce screen dirt. Toggleable per-polygon via `POLYGON_ATTR.13`.

Details with teeth:

- Comparison always uses the **W coordinate**, regardless of whether you're in Z- or W-buffering mode.
- Rendered if **at least one** vertex has W ≤ threshold — but the polygon is drawn using the colour/depth/texture of its **first** vertex.
- Hardware rounds all polygon widths and heights up to at least 1, so 0×0, 1×0, 0×1 and 1×1 all become 1×1. The check applies only to the **0×0** case — GBATEK notes "0dot" would be the better name.
- **It bypasses GXFIFO.** Changes take effect immediately and affect polygons already queued. Drain the FIFO before touching it.

### Viewport

`VIEWPORT` (cmd 60h, `4000580h`) takes X1/Y1/X2/Y2 in 0..255 / 0..191. **Coordinate 0,0 is lower-left** — inverted relative to 2D, where it's upper-left. The view volume is auto-scaled into the viewport. Vertices may exceed the X2/Y1 boundary by one pixel due to rounding. Viewport does **not** affect the rear-plane's size or position.

---

## 9. How the 3D layer behaves as BG0

From `DS 3D Final 2D Output` — this is the interface between the two documents' subject matter, and it's more restricted than either implies:

**Scrolling:** `BG0HOFS` (4000010h) scrolls the 3D layer **horizontally only**. The scroll region is 512 pixels: 256 of 3D image, then **256 transparent pixels**, then wrap. **Vertical scrolling and rotation/scaling cannot be used on the 3D layer at all.**

So Copetti's "supports horizontal scrolling" is right, and the absence of vertical is a real constraint — the 512-pixel region with a transparent half is the specific mechanism.

**Priority:** the low 2 bits of `BG0CNT` set priority relative to other BGs and OBJs, so 3D can sit in front of or behind 2D layers. **All other BG0CNT bits have no effect on 3D** — notably, **mosaic cannot be applied to the 3D layer**.

**Special effects** (4000050h–54h) work three ways:

1. Brightness up/down with BG0 as 1st target via EVY — as for 2D
2. Blending with BG0 as 2nd target via EVA/EVB — as for 2D
3. **Blending with BG0 as 1st target via per-pixel 3D alpha values** — *unlike* 2D

That third mode probably uses `EVA = A/2, EVB = 16 - A/2` per pixel, ignoring the EVA/EVB register settings. GBATEK marks this as uncertain.

**Windows** (4000040h–4Bh) work as for 2D. GBATEK flags an unverified claim that if the 3D screen has highest priority, alpha blending is always on regardless of the window colour-effect enable bit.

---

## 10. Sprite details worth having

**OBJ priority fix:** the GBA assigned OBJ priority purely by 7-bit OAM entry number, ignoring the 2-bit BG-priority attribute, which allowed invalid orderings. The DS **combines both into a 9-bit priority value**, fixing it.

**Tile OBJ mapping** (DISPCNT.4 and .20-21):

| Bit4 | Bit20-21 | Dimension | Boundary | Total |
|---|---|---|---|---|
| 0 | x | 2D | 32 | 32K (same as GBA) |
| 1 | 0 | 1D | 32 | 32K (same as GBA) |
| 1 | 1 | 1D | 64 | 64K |
| 1 | 2 | 1D | 128 | 128K |
| 1 | 3 | 1D | 256 | 256K (Engine B: 128K max) |

`TileVramAddress = TileNumber * BoundaryValue`. Changing the boundary does **not** change tile composition — OBJs remain 8×8 tiles.

**Bitmap OBJs** are selected by setting OBJ Mode (Attr 0, bits 10–11) to **3** — a value that was *prohibited* on GBA. In that mode, Attr 0 bit 13 (colour depth) should be zero, and Attr 2 bits 12–15 become an **Alpha-OAM value** instead of a palette number. Data is 15-bit direct colour with the bit-15 alpha flag.

**OBJ vertical wrap changed:** on GBA, a 64px-tall OBJ scaled to a 128px double-size region near the screen bottom wrapped to the top and was *not* drawn at the bottom. On DS it appears at **both** top and bottom. GBATEK notes this isn't strictly better — you may need to enable/disable the OBJ per screen-half at IRQ level if the wrapped portion isn't transparent.

---

## 11. Master brightness

`MASTER_BRIGHT` at `400006Ch`, per engine:

```
Brightness up:   New = Old + (63 - Old) * Factor/16
Brightness down: New = Old - Old * Factor/16
```

Factor is bits 0–4 (0–16; values above 16 clamp), mode is bits 14–15 (0 = disable, 1 = up, 2 = down).

Note it operates on **6-bit R/G/B intensities** — the panel's 18-bit depth — even though the 2D engines work in 15-bit. This is the one place the extra bit per channel is used on the 2D path, and it's why fades look smoother than a 15-bit pipeline would suggest.

---

## 12. Window glitches (hardware bugs, documented)

Two real bugs worth knowing if you're chasing rendering differences:

**Vertical:** the DS counts scanlines 0..262 (0..106h) but compares **only the lower 8 bits** against WIN0V/WIN1V. So Y1 coordinates 00h–06h also trigger in scanlines 100h–106h — the window activates during V-Blank and is active from scanline 0. Y1 = 1..6 therefore behaves as if Y1 = 0. Workarounds: disable the window during V-Blank, or set Y1 during V-Blank to a value not occurring in V-Blank (7..191).

**Horizontal:** 256 pixels don't fit in 8 bits. X1 = 00h means 0 (leftmost); X2 = 00h means 100h (rightmost). But the window isn't displayed if **X1 = X2 = 00h**, so maximum window width is **255 pixels**, not 256.

---

## 13. The display block diagram

GBATEK's ASCII diagram is the clearest statement of the whole pipeline. Reproduced structurally:

**Engine A path:** VRAM A–G feed 2D Engine A, producing OBJ, BG3, BG2, BG1 → Layering and Special Effects. Separately, VRAM A–G feed the 3D engine, whose output goes through a **selector with BG0** — that selector's output is what enters the layering unit as BG0.

The layering output then goes to a "Select Video Input and Master Brightness A" block, which chooses between:

- Layered 2D+3D output
- The capture/blend path (Select Capture Source → Blend → Select Capture Dest → back to VRAM A–D)
- **Select Display VRAM** (VRAM A–D direct, = display mode 2)
- **Main Memory Display FIFO** (DMA from main RAM, = display mode 3)

**Engine B path:** VRAM C, D, H, I feed 2D Engine B → OBJ, BG3, BG2, BG1, **BG0** → Layering → Master Brightness B. No 3D selector, no capture, no VRAM/main-memory display path.

The single most informative feature of the diagram: **BG0 on Engine A passes through a selector; on Engine B it goes straight through.** That one multiplexer is the entire 3D integration.

---

## 13a. Practitioner sources — BlocksDS and GBE+

Two further accessible sources, both confirming and extending the above.

### BlocksDS tutorial (CC BY-SA 4.0) — <https://blocksds.skylyrac.net/tutorial/>

A modern homebrew SDK's tutorial. Confirms the bank sizes exactly (A–D 128K, E 64K, F/G 16K, H 32K, I 16K) and states the workflow plainly: designate VRAM for a purpose → copy data in → point the registers at it.

Two clarifications it adds that GBATEK states only obliquely:

- **"VRAM can't be accessed by the graphics engines while in LCD mode."** LCD mode isn't a neutral default — it's CPU-exclusive. The graphics engines see nothing. This is the other half of the extended-palette restriction: you switch to LCD to write, then switch back so the engine can read.
- **Tiled graphics on the DS are always paletted.** Direct colour is a bitmap-only option. Worth internalising before planning an asset pipeline.

It also names the essential tool: **<https://mtheall.com/banks.html>** by mtheall & JustBurn — an interactive VRAM bank allocator that shows which banks can serve which purpose. Far more practical than reading the MST/OFS table by hand.

Relevant sections beyond the intro: `Backgrounds`, `Sprites`, `Scrolling big backgrounds`, `Special 2D effects`, `3D graphics`, `Advanced 3D techniques`, `Video capture`, `Optimizing code`, and the `TCM and Cache` and `DMA` chapters.

### Shonumi's NDS rolling blog — <https://shonumi.github.io/blog/nds_rolling.html>

Development log for GBE+, running 2018–2020. Complements Arisotura's write-up: where Arisotura documents *what the hardware does*, Shonumi documents *what breaks when you get it wrong*, which is often more diagnostic.

Findings worth carrying:

- **Dual-screen 3D is capture-based and 30 FPS** — see the correction in the main document §2.
- **3D-as-2D via orthographic projection is pervasive** — see main document §4.6.
- **Texture palette index 0 is treated as transparent**, the same convention as OBJ transparency on DMG/GBC/GBA and the DS 2D engine.
- **Games change the position matrix per-vertex.** *Cory in the House* does this; an emulator that applies only the last vertex's matrix to the whole polygon renders it wrong. Not something the register documentation warns you about.
- **The GX FIFO interrupt latches.** `IF.21` stays set for as long as the interrupt condition holds, or until disabled via `GXSTAT` — writing to `IF.21` to acknowledge it has no effect. Handlers must either refill the FIFO or disable the IRQ *before* acknowledging. GBATEK does state this (§8 of this document, GXSTAT bits 30–31); Shonumi confirms many games freeze without it.
- **Games may bypass the dedicated GX FIFO DMA**, using an ordinary 32-bit fixed-destination DMA to the FIFO address instead.
- **Clip Matrix Results are read by some games** (e.g. *Oshare Majo*) to recover the current position/projection matrix rather than tracking it themselves. Omitting this produces faceless, partially headless models.
- **VRAM slots are per-bank, not global.** Shonumi's initial assumption of four context-switched slots was wrong; slots are associated with specific banks. This mirrors the MST/OFS table in §7.

One caveat on this source: it's a development diary, not reference documentation. Claims are as of the date written and reflect one emulator's understanding at that moment. Cross-check anything load-bearing against GBATEK.

Shonumi also links PSI's write-up on NDS colour interpolation: <https://corgids.wordpress.com/2017/09/27/interpolation/>

---

## 14. What's still not covered here

Sections in GBATEK I haven't pulled but which are the next places to go:

- `DS 3D Texture Formats` (9348) — the compressed 4×4 format, A3I5, A5I3
- `DS 3D Texture Blending` (9468) — vertex×texture colour combination modes
- `DS 3D Toon, Edge, Fog, Alpha-Blending, Anti-Aliasing` (9542) — the tables behind §8
- `DS 3D Shadow Polygons` (9192) — the stencil mechanism in detail
- `DS 3D Polygon Attributes` (8856) and `Polygon Definitions by Vertices` (8904)
- `DS 3D Matrix Stack` (8588) and the matrix examples (8645–8855)
- `DS 3D Rear-Plane` (9811)
- `DS Files - 2D Video` (7115+) — NCLR/NCGR/NSCR asset formats

Line numbers are from the plaintext dump for direct `sed -n` access.
