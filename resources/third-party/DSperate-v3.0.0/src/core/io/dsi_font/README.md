# DSperate's DSi system font

`TWLFontTable.dat` stands in for the DSi's `/sys/TWLFontTable.dat` on a
NAND DSperate makes up (`io/dsi_nand_synth`). It is generated, not dumped:

    tools/make_dsi_font.py NotoSans-Regular.ttf wqy-microhei.ttc TWLFontTable.dat control-glyphs
    tools/make_dsi_font.py --region china NotoSans-Regular.ttf wqy-microhei.ttc TWLFontTable-cn.dat control-glyphs
    tools/make_dsi_font.py --region korea NotoSans-Regular.ttf wqy-microhei.ttc TWLFontTable-kr.dat control-glyphs

from these files (regenerate only with the same ones, or record the new ones
here):

| face | file | from | SHA-256 |
|---|---|---|---|
| Noto Sans Regular | NotoSans-Regular.ttf | Debian/Ubuntu `fonts-noto-core` 20201225-2 | `89c3c497f618fdaa0b2d1e98fef93582f28c71debd2c4a8cdf41f190ced2909d` |
| WenQuanYi Micro Hei | wqy-microhei.ttc | Debian/Ubuntu `fonts-wqy-microhei` 0.2.0-beta-3.1 | `2420e8078af796b19a3f6ef13de527a1a91c1e7171eea115926c614ced1009b3` |

Output SHA-256:

| file | console | SHA-256 |
|---|---|---|
| `TWLFontTable.dat` | Japan, Americas, Europe, Australia | `c22a62a45fc27f5d7d39307cdb19b1eb8ce20e02d573c7e69a75e50fa8952b57` |
| `TWLFontTable-cn.dat` | China | `9aa37530fe011e5350fe1e1b86754890e7b0df01c1c01851c32f61dc4ca51e27` |
| `TWLFontTable-kr.dat` | Korea | `f059ef5c151822566abbd795a97caa0605a6ae3614ba26f4e61f7040f11093b7` |

The normal table: three Nitro fonts (16x21, 12x16, 10x12 cells, 2 bpp),
7383 characters each: Latin-1 and Latin Extended-A, Greek, Cyrillic and
common symbols from Noto Sans; kana, JIS X 0208 kanji and full-width forms
from WenQuanYi Micro Hei.

The Chinese and Korean tables have the layout those consoles use: nine
resource entries, six of them zero-filled, the three fonts at 3-5
(`TBF1-cn_*`, header byte 0x86 = 4) or 6-8 (`TBF1-kr_*`, 0x86 = 5). Their
titles' SDK asks for exactly those indices and refuses them in a normal
table. China: 16x21, 12x16, 12x13 cells, 7867 characters (GB 2312 plus the
Latin, Greek and Cyrillic set). Korea: 16x21, 12x16, 12x12 cells, 3721
characters (KS X 1001 without its hanja plus the same set). The synthesised
NAND takes the table for the region the title runs in. The cell sizes are
GBATEK's; the baselines follow the normal font, as no regional console font
was measured.

`control-glyphs/` holds control button images drawn for DSperate, scaled
down to each cell size at the private-use code points DSi titles use for
them: A, B, X, Y (U+E000-E003), L and R (U+E004, U+E005),
the D-pad (U+E006) and the four arrows (U+E019 right, U+E01A left,
U+E01B up, U+E01C down). The console font's other private-use symbols
(faces, weather, Wii buttons, pointers) are not included: text using them
draws the default character.

The table carries a plain-text marker where Nintendo's RSA signature goes.
Titles check that signature through the DSi BIOS (SWI 22h); DSperate answers
that call for this marker with the header's SHA-1 (`NDS::dsi_hle_swi`). Every
hash inside the file is genuine.

Licences: `LICENSE-NotoSans-OFL-1.1.txt` (the Noto-derived glyphs are OFL
1.1), `LICENSE-WenQuanYi-MicroHei.txt` (GPL-3+ with the font exception), and
DSperate's own licence for `control-glyphs/`.
