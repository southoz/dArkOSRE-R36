#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
"""Build a synthetic Slot-1 cartridge that the DS firmware will show in its menu.

The firmware validates a card before it will draw a banner or launch it, but the
checks are cheap. It does *not* need a KEY1-encrypted secure area, so long as the
ARM9 binary sits above it (arm9_rom_offset >= 0x8000), and it never hashes the
Nintendo logo block: it only checks that the header's logo CRC *field* holds the
constant 0xCF56. A zeroed logo with that field set is accepted and launches, so
nothing here has to be copied out of a real dump. (--logo-from remains available
for a byte-faithful card; a zeroed block with a self-consistent CRC over the zeros
is rejected, which is what shows the field is all the firmware reads.)

The stubs here only spin: the point is to be tapped, not to run -- the firmware
fades to white and then stops touching the cart bus altogether, so the stubs are
never actually loaded. That fade is what the frontend watches for.

  tools/mkcart.py --icon icon.png --title "DSperate" --subtitle "Choose a game"

The default output name, BootMenu.nds, is the one the SDL frontend already treats
as "boot the firmware": drop the card next to the config and it is put in the slot
under a firmware boot, so the DS menu draws its banner.
"""
import argparse, struct, sys

BANNER_OFF, ARM9_OFF, ARM7_OFF = 0x9000, 0x8000, 0x8200
ROM_SIZE = 0x20000


def crc16(data, init=0xFFFF):
    v = init
    for b in data:
        v ^= b
        for _ in range(8):
            carry = v & 1
            v >>= 1
            if carry:
                v ^= 0xA001
    return v & 0xFFFF


def rgb555(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def icon_from_image(path):
    """32x32, 4bpp, 16 colours. Index 0 is transparent, so a source image with an
    alpha channel keeps its cut-out shape and only 15 colours are available for
    the visible part."""
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    if im.size != (32, 32):
        im = im.resize((32, 32), Image.LANCZOS)
    alpha = im.getchannel("A")
    transparent = alpha.point(lambda a: 255 if a < 128 else 0)
    # Quantise the visible pixels to 15 colours, then shift every index up by one
    # so that palette slot 0 is free to mean transparent.
    flat = Image.new("RGBA", im.size, (0, 0, 0, 255))
    flat.paste(im, mask=alpha)
    pal_im = flat.convert("RGB").quantize(colors=15, method=Image.MEDIANCUT)
    src = pal_im.load()
    tp = transparent.load()
    px = [[0] * 32 for _ in range(32)]
    for y in range(32):
        for x in range(32):
            px[y][x] = 0 if tp[x, y] else src[x, y] + 1
    # An image with fewer than 15 distinct colours quantises to a shorter
    # palette, so take however many came back rather than assuming 15; the
    # unused slots are padded out below and no pixel indexes them.
    raw = pal_im.getpalette()[: 15 * 3]
    palette = [0] + [rgb555(raw[i], raw[i + 1], raw[i + 2]) for i in range(0, len(raw), 3)]
    return px, palette + [0] * (16 - len(palette))


def icon_placeholder():
    """A plain framed square, so the generator works with no image and no PIL."""
    px = [[0] * 32 for _ in range(32)]
    for y in range(32):
        for x in range(32):
            if 3 <= x < 29 and 3 <= y < 29:
                px[y][x] = 1 if (6 <= x < 26 and 6 <= y < 26) else 2
                if 10 <= x < 22 and 10 <= y < 22:
                    px[y][x] = 3
    return px, [0x0000, 0x7FFF, 0x001F, 0x7C00] + [0] * 12


def tiles(px):
    """The banner's 512 bytes of icon: sixteen 8x8 tiles, row-major, 4bpp with
    the low nibble first."""
    out = bytearray(0x200)
    for ty in range(4):
        for tx in range(4):
            base = (ty * 4 + tx) * 32
            for row in range(8):
                for col in range(0, 8, 2):
                    lo = px[ty * 8 + row][tx * 8 + col]
                    hi = px[ty * 8 + row][tx * 8 + col + 1]
                    out[base + row * 4 + col // 2] = lo | (hi << 4)
    return out


def build_banner(px, palette, title):
    ban = bytearray(0xA40)
    struct.pack_into("<H", ban, 0x00, 1)          # version 1
    ban[0x20:0x220] = tiles(px)
    for i, c in enumerate(palette):
        struct.pack_into("<H", ban, 0x220 + i * 2, c)
    # Six language slots of 0x100 bytes each, UTF-16LE. The menu shows at most
    # three lines; a longer title is simply clipped by the firmware.
    text = title.encode("utf-16-le")[: 0x100 - 2]
    for lang in range(6):
        off = 0x240 + lang * 0x100
        ban[off : off + len(text)] = text
    struct.pack_into("<H", ban, 0x02, crc16(ban[0x20:0x840]))
    return ban


def stub(magic):
    # str r1,[r0] ; b . -- then the two literals it loads.
    words = [0xE59F0008, 0xE59F1008, 0xE5801000, 0xEAFFFFFE, 0x04FFFFF0, magic]
    return b"".join(struct.pack("<I", w) for w in words)


def build(logo_src, icon_path, title, game_code, out):
    rom = bytearray(ROM_SIZE)

    px, palette = icon_from_image(icon_path) if icon_path else icon_placeholder()
    banner = build_banner(px, palette, title)

    h = bytearray(0x200)
    h[0x00:0x0C] = title.split("\n")[0].upper().encode("ascii", "replace")[:12].ljust(12, b"\0")
    h[0x0C:0x10] = game_code.encode("ascii")[:4].ljust(4, b"\0")
    h[0x10:0x12] = b"01"
    h[0x12] = 0x00                                 # unit code: NDS
    h[0x14] = 0x09                                 # device capacity
    struct.pack_into("<IIII", h, 0x20, ARM9_OFF, 0x02000000, 0x02000000, 0x40)
    struct.pack_into("<IIII", h, 0x30, ARM7_OFF, 0x02380000, 0x02380000, 0x40)
    struct.pack_into("<I", h, 0x68, BANNER_OFF)
    struct.pack_into("<H", h, 0x6E, 0x0D7E)        # secure area delay
    struct.pack_into("<I", h, 0x80, ROM_SIZE)
    struct.pack_into("<I", h, 0x84, 0x4000)
    # With no source the logo block stays zeroed; either way the CRC field must
    # read 0xCF56, which is the only thing the firmware compares.
    if logo_src is not None:
        h[0xC0 : 0xC0 + 156] = logo_src[0xC0 : 0xC0 + 156]
    struct.pack_into("<H", h, 0x15C, 0xCF56)
    struct.pack_into("<H", h, 0x15E, crc16(h[0x00:0x15E]))

    rom[0:0x200] = h
    rom[ARM9_OFF : ARM9_OFF + 24] = stub(0x44535039)
    rom[ARM7_OFF : ARM7_OFF + 24] = stub(0x44535037)
    rom[BANNER_OFF : BANNER_OFF + len(banner)] = banner
    open(out, "wb").write(bytes(rom))
    return len(rom)


def emit_header(px, palette, path, icon_path):
    """The icon alone, as the C++ frontend's built-in default (loader_icon.h).
    Only the pixels and the palette are generated: the rest of the cart is built
    in loader_cart.cpp, which is this file's build() ported."""
    t = tiles(px)
    with open(path, "w") as f:
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n"
                "// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.\n"
                "//\n"
                "// GENERATED by tools/mkcart.py --emit-header; do not edit.\n"
                f"// Icon: {icon_path}\n"
                "#pragma once\n"
                "#include \"core/types.h\"\n\n"
                "namespace ds::sdl {\n\n"
                "// 32x32, sixteen 8x8 4bpp tiles, row-major; palette index 0 is transparent.\n"
                "inline constexpr u8 kLoaderIconTiles[0x200] = {\n")
        for i in range(0, len(t), 16):
            f.write("    " + " ".join(f"0x{b:02x}," for b in t[i : i + 16]) + "\n")
        f.write("};\n\ninline constexpr u16 kLoaderIconPalette[16] = {\n    ")
        f.write(" ".join(f"0x{c:04x}," for c in palette))
        f.write("\n};\n\n} // namespace ds::sdl\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--logo-from", metavar="ROM",
                    help="any .nds dump, to copy a byte-faithful Nintendo logo block; "
                         "omit it and the block is left zeroed, which the firmware "
                         "accepts just as readily")
    ap.add_argument("--icon", metavar="IMAGE",
                    help="icon image (any size, resized to 32x32; alpha becomes "
                         "the transparent index, leaving 15 colours)")
    ap.add_argument("--title", default="DSperate", help="first banner line")
    ap.add_argument("--subtitle", default="Choose a game",
                    help="second banner line; pass an empty string for one line")
    ap.add_argument("--game-code", default="#DSP")
    ap.add_argument("--emit-header", metavar="PATH",
                    help="write the icon out as the frontend's built-in default "
                         "(src/frontend/sdl/loader_icon.h) instead of a cart")
    ap.add_argument("-o", "--out", default="BootMenu.nds",
                    help="output path; the default name is what the frontend looks for")
    a = ap.parse_args()

    if a.logo_from:
        logo = open(a.logo_from, "rb").read(0x200)
        if len(logo) < 0x160:
            sys.exit(f"{a.logo_from}: too short to be a ROM")
    else:
        logo = None
    title = a.title + ("\n" + a.subtitle if a.subtitle else "")
    if a.emit_header:
        px, palette = icon_from_image(a.icon) if a.icon else icon_placeholder()
        emit_header(px, palette, a.emit_header, a.icon or "(placeholder)")
        print(f"wrote {a.emit_header}")
        return
    n = build(logo, a.icon, title, a.game_code, a.out)
    print(f"wrote {a.out}, {n} bytes, icon "
          f"{'from ' + a.icon if a.icon else '(placeholder)'}")


if __name__ == "__main__":
    main()
