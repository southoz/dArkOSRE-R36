# DraStic's game-specific behaviour — a list to review

Compiled 2026-08-29 from the Ghidra decompilation (`apply_cycle_adjustment_hacks`
at `0x0010fa20` and the readers of its flag block) and from the changelog in
DraStic's bundled `drastic_readme.txt`. Two different things live here and
they are kept apart: **(1) the per-game hack table** — fourteen titles keyed on the
ROM game code, each flipping one of eight flags — and **(2) general fixes the
changelog attributes to a particular game**, which are not per-game code paths at
all but bugs that one title happened to expose. Neither Golden Sun: Dark Dawn nor
Dragon Ball Origins appears in (1); DraStic's speed on them is its design, not a tune.

## 1. The hack table (`apply_cycle_adjustment_hacks`, keyed on the low 24 bits of the game code)

The flag block sits in `nds_system_struct` (decompiled as `exit_jmp_buf[0x148..0x160]`);
every flag is cleared at reset except the gamecard multiplier, which defaults to 1.

| flag | consumer | meaning | default |
|---|---|---|---|
| `+0x148` (u32) | `cpu_translate_block` | extra ARM9 cycles added per instruction to the system-wide adjustment word (01 §2) | 0 |
| `+0x154` (u32) | geometry | **GXFIFO command timing multiplier** | **0 — geometry commands cost nothing** |
| `+0x158` (u32) | DMA | **DMA timing multiplier** over the static seq/non-seq tables (04 §4) | **0 — DMA is instant** |
| `+0x15c` (u32) | `gamecard_command`, `dma_transfer_gamecard` | gamecard transfer timing: `words × 0x28 × flag` cycles to the IRQ | 1 |
| `+0x15d` (bool) | `event_hblank_start` | at line 191's HBlank, VCOUNT reads 192 (both CPUs) before the IRQ fires | off |
| `+0x15e` (bool) | DMA | "DMA CPU": DMA time charged to the CPU (set only with Yu-Gi-Oh) | off |
| `+0x15f` (bool) | geometry | "swap stalls geometry": SWAP_BUFFERS stalls the command stream (the FIFO-pace behaviour) | off |
| `+0x160` (bool) | `event_hblank_start` | "force undeferred 2D": `video_render_scanlines` runs per scanline (lazy 2D off) | off |

The defaults are the notable part. The changelog (2.1.6a era) says it outright:
*"Rolled back to not using DMA or gxfifo timings except in Bowser's Inside Story.
Fixes a bunch of regressions this caused (and improves performance)."* So DraStic's
shipping DMA and geometry are untimed for every title but one, and gamecard
timing is a single multiplier. DSperate's per-unit DMA (4.8/4.9), per-command
geometry timing (item 3) and cart model are all exact — that is the divergence
that costs us in the main-thread profiles ([gsdd-device-profile] in memory), and
it is a deliberate one.

| game (code) | hack | changelog note |
|---|---|---|
| Mario & Luigi: Bowser's Inside Story (`CLJ`) | +2 cycles, DMA ×2, **geometry ×4** | "gxfifo timing and some timing hacks … reduces problems"; the only title with DMA/GXFIFO timing on |
| Yu-Gi-Oh! 5D's WC 2010 (`CY8`) | +1 cycle, DMA ×1, DMA charged to CPU | "Changed DMA timing … preventing a freeze in battle" |
| Element Hunters (`BEL`) | +1 cycle | — |
| Zhu Zhu Babies, Europe only (`B5J` + region `P`) | +1 cycle | "timing fixes … Zhu Zhu Babies and Will o' Wisp DS" |
| Will o' Wisp DS | +1 cycle | same entry |
| Ore ga Omae o Mamoru (`CVJ`) | +1 cycle | "workaround for hang" |
| Puppy Palace (`YPT`) | +2 cycles | "workaround for hang" |
| Legend of Kay | +1 cycle | "workaround for hang" |
| Sonic Chronicles | +1 cycle | "Changed timing for Sonic Chronicles. Fixes crashes." |
| Spider-Man: Shattered Dimensions | +1 cycle | "workaround for hang" |
| Art Academy (`VAA`) | VCOUNT = 192 at line 191 | "so it'd read vcount=192 before the IRQ fires and gets past its load screen. Looking into less hacky alternatives." |
| Florist Shop (`B2F`) | force undeferred 2D | "workaround that turns off deferred rendering" |
| Imagine: Champion Rider (`CH4`) | swap stalls geometry | — |
| American Girl games | swap stalls geometry | "Added geometry stalls, fixing graphical problems in American Girl games" |

What to take from it for DSperate:
- The ten "+1/+2 cycle" entries are all hangs or crashes in games sensitive to
  ARM9 timing under an *inexact* cycle model. Our model is the interpreter's
  per-page timing table (1.8 `equiv`) — worth testing those ten titles cold,
  since if they run we have no reason to carry the table; if any hangs, the
  fix should be in the model, not a table.
- `swap stalls geometry` and `force undeferred 2D` are DraStic patching around
  its own deferrals (frame-granular geometry replay, VBlank-batched 2D). We
  execute geometry at FIFO pace (item 3) and catch mid-frame VRAM stores with
  the trap, so neither case needs a switch here; American Girl / Champion Rider
  / Florist Shop are cheap confirmation tests of that claim.
- The Art Academy VCOUNT hack is the one genuine oddity: worth a look at what
  our VCOUNT reads at line 191's HBlank versus hardware before deciding.

## 2. Changelog fixes attributed to a game (not per-game code)

These are the titles the readme names as having exposed a bug; each is a
general fix. Grouped by subsystem so they can be turned into a test list.

**2D / capture / VRAM mapping** — Doki Doki Majo (capture to invalid VRAM banks);
Puyo Pop Fever, WarioWare DIY, FF Tactics A2 (sprites); Daniel X (bank mirroring);
Zelda Phantom Hourglass (2D engine screen switch mid-frame); Spyro (huge BGs);
Holly Hobby (2D corruption); Knytt Stories (screen transition); Lock's Quest
(palette DMA from gamecard mid-frame); Pokémon HG/SS (page unmapper); Pokémon
(palette/OAM mapping init, "blacked out graphics"); GTA (VRAM mapping);
Splinter Cell night vision, Need for Speed Carbon (capture); Golden Sun ("screen
glitches", a VRAM/texture accommodation); Bob's Game (8bpp affine sprite MSB);
Chrono Trigger (extreme affine zoom crash); Mario Kart (VRAM map init on state load).

**3D** — American Girl (viewport values; geometry stalls); Marvel Super Hero Squad
(Z/W latch delay); Zelda PH (depth equality); FF4 (Z precision; texture cache; sort
regression); SaGa 3 (same texture, different repeat modes); Fossil Fighters (3D layer
horizontal offset); Dark Spire, Tales of Innocence (clear colour/depth caching);
Dragon Quest IX (3D-layer blending); Zelda (alpha ID test in NEON); Justice League
(alpha ID on opaque pixels); Animal Crossing, Zelda ST (fog green dots); Advance Wars
(toon highlight modulates R); Cars (highlight shading); Mario Kart (toon on untextured);
GTA:CW (light vector cache); Nanostray 2 (direction matrix reads); Okamiden (clip
overflow); Fire Emblem: Dark Shadow (1-element texture matrix stack); Heroes of Mana,
Glory of Heracles, Assassin's Creed, Inazuma Eleven (redundant glBegin); Last Window,
Hotel Dusk, Zelda PH (null-texture caching); "y-sort for polygons" (menus generally).

**CPU / recompiler** — Minecraft DS, Pokémon White (translation buffer size);
Inazuma Eleven 2 (conditional → unconditional conversion); Penguins of Madagascar
(conditional str); Super Robot Wars Exceed patch (conditional swi); Infinite Space
(swp); Front Mission, de Blob 2, Panzer Tactics (register allocation); Donkey Kong
Jungle Climber (event check after stm); Spore Creatures (ldm); Pokémon B/W2 (bad
branch offset with C-Gear; halt with I flag set); Megaman BN5 (flags caching);
LEGO Harry Potter (flags); Eragon (sqrt/div); Pokémon "mark" cheats (0x0 terminator).

**Timing / cart / IPC / misc** — Pokémon HG/SS/Platinum (gamecard + DMA timing,
"at least playable due to a timing hack"; backup command 0x8; extra cycles for
unconditional block memory instructions); Digimon World / Digimon Story (slot-2
reads); GTA:CW (DMA re-issue hang); Big Bang Mini (IPC flags); Tangled (sleep exit);
Cooking Mama (BIOS GetCRC16 zero length); Brain Training (ARM7 behaviour); Knights
in the Nightmare (DMA from ITCM reads zero); Pokémon (RTC October; RTC parameter
counts); Pokémon Mystery Dungeon EoS (slowdown; 128 KB EEPROM); Pokémon Conquest
(gamecard address wraparound / anti-piracy); Dementium II (AUXSPICNT status bit);
Fire Emblem (PSG), Rune Factory (voice looping), jEnesisDS (sample buffers outside
main RAM), Chrono Trigger (custom BIOS audio pitch/volume), Golden Sun DD (audio
after savestate load).

## 3. Suggested next step

Turn §1's fourteen titles plus the §2 capture/timing entries into a boot-and-run
smoke list (a few hundred frames each, hash + "did it hang") against the exact
model, before considering any per-game table of our own. The interesting outcomes
are the ones where the exact model *also* fails — those are model bugs — and the
ones DraStic needed a hack for that we don't, which are worth writing down as
evidence for the exact-model choice.
