#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build DSperate's own DSi system font, /sys/TWLFontTable.dat.

    make_dsi_font.py [--region normal|china|korea] NotoSans-Regular.ttf wqy-microhei.ttc out.dat [control-glyphs/]

A console's table depends on its region. Japan, the Americas, Europe and
Australia ("normal") carry three fonts as resources 0-2. China and Korea
carry nine resource entries: the first six zero-filled except for their own
three, at 3-5 (China) or 6-8 (Korea), with byte 0x86 of the header saying
which (4 or 5). The TWL SDK's OS_LoadSharedFont refuses an index at or past
the entry count, and one of 3 or more while that byte is zero, so a title
built for one region cannot use another region's table.

DSiWare that draws text with the console's shared font reads it from the
NAND. With no NAND dump DSperate supplies this one instead: the same table
layout (GBATEK "DSi SD/MMC Firmware Font File"), three Nitro fonts (16x21,
12x16 and 10x12 cells, 2 bits per pixel) compressed with the DSi's
backwards LZ, rasterised from

  - Noto Sans (SIL Open Font License 1.1) for Latin, Greek, Cyrillic and
    general symbols, and
  - WenQuanYi Micro Hei (GPL-3+ with the font exception, or Apache-2.0) for
    kana, kanji (JIS X 0208) and full-width forms; for China the characters
    of GB 2312, for Korea those of KS X 1001 without its hanja (Hangul,
    jamo, symbols), as the consoles' own Chinese (7848) and Korean (3679)
    fonts have about that many.

The characters are the ones those scripts need, not a copy of Nintendo's
table. Of Nintendo's private-use symbols only the control buttons are
included, drawn for DSperate (io/dsi_font/control-glyphs, the fourth
argument) and put at the code points titles use for them (CONTROL_GLYPHS). The
table's RSA signature cannot be made, so the first 0x80 bytes hold a plain
marker instead; the emulator recognises it when a title checks the signature
through the DSi BIOS (SWI 22h) and answers with this file's header digest.
Every SHA-1 in the file is real. Needs Pillow and fontTools.
"""
import hashlib, struct, sys
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont, TTCollection

SIGNATURE_MARKER = b'DSperate TWLFontTable.dat: generated, not signed by Nintendo'.ljust(0x80, b'\0')
DATE = bytes([0x00, 0x13, 0x09, 0x26])     # the layout's date field, BCD, as in 2026-09-13

# name, cell width, cell height, baseline row (the first row below the text),
# the widest advance, and the two faces' pixel sizes. Cell geometry follows
# what DSi titles are written against; the pixel sizes put a capital 13/10/8
# pixels tall and a kanji 14/11/10.
SIZES = [
    ('TBF1_l.NFTR', 16, 21, 17, 17, 18, 15),
    ('TBF1_m.NFTR', 12, 16, 13, 13, 14, 12),
    ('TBF1_s.NFTR', 10, 12, 10, 11, 11, 10),
]

# The regional tables: resource names, where they sit among the nine
# entries, the header's region byte, and their cell sizes. GBATEK gives the
# cells (the small font is 12x13 in China and 12x12 in Korea); the baselines
# and pixel sizes follow the normal font's proportions, as no regional font
# was measured.
REGIONS = {
    'normal': (0, 3, 0, SIZES),
    'china': (3, 9, 4, [
        ('TBF1-cn_l.NFTR', 16, 21, 17, 17, 18, 15),
        ('TBF1-cn_m.NFTR', 12, 16, 13, 13, 14, 12),
        ('TBF1-cn_s.NFTR', 12, 13, 11, 13, 11, 12),
    ]),
    'korea': (6, 9, 5, [
        ('TBF1-kr_l.NFTR', 16, 21, 17, 17, 18, 15),
        ('TBF1-kr_m.NFTR', 12, 16, 13, 13, 14, 12),
        ('TBF1-kr_s.NFTR', 12, 12, 10, 13, 11, 11),
    ]),
}


# Private-use code points (as the console's font assigns them) and the image
# drawn for each; the largest image of a name is the one scaled down. `wide`
# glyphs keep their 3:2 shape.
CONTROL_GLYPHS = [
    (0xE000, 'A_light_dark', False), (0xE001, 'B_light_dark', False),
    (0xE002, 'X_light_dark', False), (0xE003, 'Y_light_dark', False),
    (0xE004, 'LB_light', True), (0xE005, 'RB_light', True),
    (0xE006, 'Digipad_light', False),
    (0xE019, 'Right_light', False), (0xE01A, 'Left_light', False),
    (0xE01B, 'Up_light', False), (0xE01C, 'Down_light', False),
]


def load_control_glyphs(folder):
    """{code point: (ink map as an 'L' image, 0 = none .. 255 = solid, wide)}."""
    import glob, os
    out = {}
    for cp, stem, wide in CONTROL_GLYPHS:
        files = glob.glob(os.path.join(folder, stem + '*.png'))
        if not files:
            sys.exit(f'{folder}: no {stem}*.png')
        img = max((Image.open(f) for f in files), key=lambda i: i.width * i.height).convert('RGBA')
        # Ink is what is dark and opaque; white or transparent is background.
        ink = Image.new('L', img.size, 0)
        ink.putdata([int((255 - (r * 299 + g * 587 + b * 114) // 1000) * a / 255) for r, g, b, a in img.getdata()])
        box = ink.getbbox()
        out[cp] = (ink.crop(box) if box else ink, wide)
    return out


def render_control(ink, wide, w, h, base):
    """A control glyph fitted to the kanji box: bottom on the baseline row,
    as tall as the cell allows above it and one pixel narrower than the cell."""
    size = min(w - 1, base - 2)
    gw, gh = (size, max(1, round(size * ink.height / ink.width))) if wide else (size, size)
    small = ink.resize((gw, gh), Image.BOX)
    top = base + 1 - gh - ((size - gh) // 2 if wide else 0)
    level = [min(3, int(((v / 255) ** 0.7) * 3 + 0.5)) for v in range(256)]
    rows = [[0] * w for _ in range(h)]
    for y in range(gh):
        for x in range(gw):
            if 0 <= top + y < h:
                rows[top + y][x] = level[small.getpixel((x, y))]
    return rows, 1, gw, gw + 2


def double_byte(codec, leads, trails):
    """The single characters a double-byte code page encodes."""
    out = set()
    for lead in leads:
        for trail in trails:
            try:
                ch = bytes([lead, trail]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ord(ch))
    return out


def coverage(region='normal'):
    cps = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180))
    cps |= set(range(0x384, 0x3CF)) | set(range(0x400, 0x460))            # Greek, Cyrillic
    cps |= {0x192, 0x2C6, 0x2C7, 0x2D8, 0x2D9, 0x2DA, 0x2DB, 0x2DC, 0x2DD}
    cps |= set(range(0x2010, 0x2027)) | {0x2030, 0x2032, 0x2033, 0x2039, 0x203A, 0x203B, 0x2044, 0x20AC}
    cps |= {0x2103, 0x2116, 0x2122} | set(range(0x2160, 0x216C)) | set(range(0x2190, 0x219A)) | {0x21D2, 0x21D4}
    cps |= set(range(0x3000, 0x3040)) | set(range(0x3041, 0x3097)) | set(range(0x309B, 0x30A0)) | set(range(0x30A1, 0x3100))
    cps |= set(range(0xFF01, 0xFF5F)) | set(range(0xFFE0, 0xFFE7))
    if region == 'china':
        # GB 2312: every EUC-CN double-byte code (symbols, pinyin, both hanzi levels).
        cps |= double_byte('gb2312', range(0xA1, 0xF8), range(0xA1, 0xFF))
    elif region == 'korea':
        # KS X 1001 without the hanja: Hangul syllables, jamo and the symbol rows.
        cps |= {c for c in double_byte('euc_kr', range(0xA1, 0xFF), range(0xA1, 0xFF))
                if not (0x4E00 <= c <= 0x9FFF or 0xF900 <= c <= 0xFAFF)}
    else:
        cps |= set(range(0xFF61, 0xFFA0))   # half-width katakana
        # JIS X 0208: every double-byte Shift-JIS code (symbols, kana, both kanji levels).
        cps |= double_byte('shift_jis', list(range(0x81, 0xA0)) + list(range(0xE0, 0xF0)), list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)))
    return sorted(c for c in cps if not 0xE000 <= c <= 0xF8FF)


def cjk(c):
    return (0x1100 <= c <= 0x11FF or 0x2E80 <= c <= 0x9FFF or 0xAC00 <= c <= 0xD7A3 or
            0xF900 <= c <= 0xFAFF or 0xFF00 <= c <= 0xFFEF)


def cmap_of(path):
    if path.lower().endswith('.ttc'):
        return TTCollection(path).fonts[0].getBestCmap()
    return TTFont(path).getBestCmap()


def render(face, c, w, h, base, shift, fullwidth):
    """The glyph as (bitmap rows of 0..3, left bearing, ink width, advance)."""
    img = Image.new('L', (w * 3, h), 0)
    ImageDraw.Draw(img).text((w, base + shift), chr(c), font=face, fill=255, anchor='ls')
    px = img.load()
    cols = [x for x in range(img.width) if any(px[x, y] for y in range(h))]
    advance = w if fullwidth else max(1, int(round(face.getlength(chr(c)))))
    if not cols:
        return [[0] * w for _ in range(h)], 0, 0, min(advance, 255)
    left, right = cols[0], min(cols[-1], cols[0] + w - 1)
    # Four levels, with a gamma lift so antialiased stems stay as dark as the
    # console's own font draws them at these sizes.
    level = [min(3, int(((v / 255) ** 0.7) * 3 + 0.5)) for v in range(256)]
    rows = [[level[px[x, y]] if x <= right and x < img.width else 0 for x in range(left, left + w)] for y in range(h)]
    return rows, left - w, right - left + 1, min(advance, 255)


def pad4(b):
    return b + b'\0' * (-len(b) % 4)


def build_nftr(codes, faces, w, h, base, maxw, controls):
    tile_bytes = (w * h * 2 + 7) // 8
    glyphs, widths = bytearray(), bytearray()
    for c in codes:
        if c in controls:
            rows, left, ink, adv = render_control(*controls[c], w, h, base)
        else:
            face, shift, full = faces(c)
            rows, left, ink, adv = render(face, c, w, h, base, shift, full)
        bits = 0
        for y in range(h):
            for x in range(w):
                bits = (bits << 2) | rows[y][x]
        glyphs += (bits << (tile_bytes * 8 - w * h * 2)).to_bytes(tile_bytes, 'big')
        widths += struct.pack('<bBB', max(-128, min(127, left)), ink, adv)

    # Character maps: a type-0 run for each block of 12 or more consecutive
    # code points (their tiles are consecutive too), one sorted type-2 map for
    # the rest.
    runs, singles, i = [], [], 0
    while i < len(codes):
        j = i
        while j + 1 < len(codes) and codes[j + 1] == codes[j] + 1:
            j += 1
        if j - i + 1 >= 12:
            runs.append((codes[i], codes[j], i))
        else:
            singles += [(codes[k], k) for k in range(i, j + 1)]
        i = j + 1

    cmap_bodies = [struct.pack('<HHI', first, last, 0) + pad4(struct.pack('<H', tile)) for first, last, tile in runs]
    cmap_bodies.append(struct.pack('<HHI', 0x0000, 0xFFFF, 2) +
                       pad4(struct.pack('<H', len(singles)) + b''.join(struct.pack('<HH', c, t) for c, t in singles)))

    finf_at = 0x10
    cglp_at = finf_at + 0x20
    cglp = pad4(struct.pack('<BBHBBBB', w, h, tile_bytes, base, maxw, 2, 0) + bytes(glyphs))
    cwdh_at = cglp_at + 8 + len(cglp)
    cwdh = pad4(struct.pack('<HHI', 0, len(codes) - 1, 0) + bytes(widths))
    cmap_at = cwdh_at + 8 + len(cwdh)

    out = bytearray()
    finf = struct.pack('<BBHBBBB', 0, h, 0, 0, w, w, 1) + struct.pack('<III', cglp_at + 8, cwdh_at + 8, cmap_at + 8) + bytes([h, maxw, base, 0])
    out += b'FNIF' + struct.pack('<I', 8 + len(finf)) + finf
    out += b'PLGC' + struct.pack('<I', 8 + len(cglp)) + cglp
    out += b'HDWC' + struct.pack('<I', 8 + len(cwdh)) + cwdh
    at = cmap_at
    for k, body in enumerate(cmap_bodies):
        size = 8 + 4 + len(body)
        nxt = at + size + 8 if k + 1 < len(cmap_bodies) else 0
        out += b'PAMC' + struct.pack('<I', size) + body[:8] + struct.pack('<I', nxt) + body[8:]
        at += size
    total = 0x10 + len(out)
    return struct.pack('<4sHHIHH', b'RTFN', 0xFEFF, 0x0102, total, 0x10, 3 + len(cmap_bodies)) + bytes(out)


# ---- the DSi's backwards LZ ------------------------------------------------------

def blz_decode(data):
    n = len(data)
    inc = struct.unpack_from('<I', data, n - 4)[0]
    if inc == 0:
        return bytes(data)
    hdr = data[n - 5]
    enc = data[n - 8] | data[n - 7] << 8 | data[n - 6] << 16
    raw = bytearray(n + inc)
    raw[:n - enc] = data[:n - enc]
    pak, pak_end, r, r_end = n - hdr, n - enc, len(raw), n - enc
    mask = flags = 0
    while r > r_end:
        mask >>= 1
        if not mask:
            pak -= 1; flags = data[pak]; mask = 0x80
        if not flags & mask:
            pak -= 1; r -= 1; raw[r] = data[pak]
        else:
            pak -= 2; pos = data[pak + 1] << 8 | data[pak]
            ln = min((pos >> 12) + 3, r - r_end)
            pos = (pos & 0xFFF) + 3
            for _ in range(ln):
                r -= 1; raw[r] = raw[r + pos]
    return bytes(raw)


def blz_encode(raw):
    """Compress `raw` so it decompresses in place: LZ77 over the reversed
    data, reversed back, with an uncompressed head long enough that the
    decoder's output never overtakes its unread input."""
    rev = raw[::-1]
    n = len(rev)
    heads = {}
    stream = bytearray()      # the forward token stream over `rev`
    marks = [(0, 0)]          # (input consumed, output written) at each token end
    i = 0
    flag_at, bit = -1, 0
    while i < n:
        if bit == 0:
            flag_at = len(stream); stream.append(0); bit = 0x80
        best_len, best_disp = 0, 0
        if i + 3 <= n:
            key = rev[i:i + 3]
            for p in reversed(heads.get(key, ())):
                disp = i - p
                if disp > 0x1002:
                    break
                if disp < 3:
                    continue
                ln = 3
                while ln < 18 and i + ln < n and rev[p + ln] == rev[i + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_disp = ln, disp
                    if ln == 18:
                        break
        step = best_len if best_len >= 3 else 1
        if best_len >= 3:
            stream[flag_at] |= bit
            v = (best_len - 3) << 12 | (best_disp - 3)
            stream += bytes([v >> 8, v & 0xFF])
        else:
            stream.append(rev[i])
        for k in range(i, i + step):
            if k + 3 <= n:
                lst = heads.setdefault(rev[k:k + 3], [])
                lst.append(k)
                if len(lst) > 16:
                    del lst[0]
        i += step
        bit >>= 1
        marks.append((i, len(stream)))
    # Cut at the token where consumed-minus-written reaches its running
    # maximum: everything before it (in raw order, the head) stays plain.
    best_k, best_s = 0, -1
    for k, (inp, outp) in enumerate(marks):
        s = inp - outp
        if s >= best_s:
            best_s, best_k = s, k
    inp, outp = marks[best_k]
    comp = bytes(stream[:outp])[::-1]
    # A flag byte whose group was cut short is fine: the decoder stops at the
    # head before reading the missing tokens.
    head = raw[:n - inp]
    body = head + comp
    footer_len = 8 + (-len(body) % 4)
    enc_len = len(comp) + footer_len
    total = len(body) + footer_len
    out = body + b'\xFF' * (footer_len - 8) + struct.pack('<I', enc_len)[:3] + bytes([footer_len]) + struct.pack('<I', len(raw) - total)
    assert len(raw) > total, 'incompressible'
    return out


def check_in_place(packed, raw_len):
    """Decode in one buffer the way an in-place decoder would; the output
    must never overwrite compressed bytes not yet read."""
    n = len(packed)
    inc = struct.unpack_from('<I', packed, n - 4)[0]
    if inc == 0:
        return True
    hdr = packed[n - 5]
    enc = packed[n - 8] | packed[n - 7] << 8 | packed[n - 6] << 16
    pak, pak_end, r = n - hdr, n - enc, n + inc
    mask = flags = 0
    while r > pak_end:
        mask >>= 1
        if not mask:
            pak -= 1; flags = packed[pak]; mask = 0x80
        if not flags & mask:
            pak -= 1; r -= 1
        else:
            pak -= 2; ln = min((((packed[pak + 1] << 8) | packed[pak]) >> 12) + 3, r - pak_end); r -= ln
        if r < pak:
            return False
    return True


def main(noto_path, cjk_path, out_path, glyph_dir=None, region='normal'):
    first, count, region_byte, sizes = REGIONS[region]
    noto_cmap, cjk_cmap = cmap_of(noto_path), cmap_of(cjk_path)
    controls = load_control_glyphs(glyph_dir) if glyph_dir else {}
    codes = sorted(set(c for c in coverage(region) if c in noto_cmap or c in cjk_cmap) | set(controls))
    entries, blobs = [bytes(0x40)] * first, []
    offset = 0xA0 + 0x40 * count
    for name, w, h, base, maxw, latin_px, cjk_px in sizes:
        noto = ImageFont.truetype(noto_path, latin_px)
        wqy = ImageFont.truetype(cjk_path, cjk_px, index=0)
        # Put a kanji's ink bottom on the baseline row, as the Latin capitals' is.
        probe = Image.new('L', (w * 3, h * 2), 0)
        ImageDraw.Draw(probe).text((w, h), '漢', font=wqy, fill=255, anchor='ls')
        bottom = max(y for y in range(h * 2) if any(probe.getpixel((x, y)) for x in range(w * 3)))
        cjk_shift = h - bottom                 # its ink bottom on the baseline row itself
        def faces(c):
            use_cjk = (cjk(c) and c in cjk_cmap) or c not in noto_cmap
            return (wqy, cjk_shift, cjk(c) and not 0xFF61 <= c <= 0xFFDC) if use_cjk else (noto, 0, False)
        nftr = build_nftr(codes, faces, w, h, base, maxw, controls)
        packed = blz_encode(nftr)
        assert blz_decode(packed) == nftr, name + ': the compressor does not round-trip'
        assert check_in_place(packed, len(nftr)), name + ': not safe to decompress in place'
        entries.append(name.encode().ljust(0x20, b'\0') + struct.pack('<III', len(packed), offset, len(nftr)) + hashlib.sha1(packed).digest())
        blobs.append(packed + b'\0' * (-len(packed) % 16))
        print(f'{name}: {len(codes)} characters, {len(nftr)} bytes, {len(packed)} compressed', file=sys.stderr)
        offset += len(blobs[-1])
    entries += [bytes(0x40)] * (count - len(entries))
    table = b''.join(entries)
    header = DATE + bytes([count, 0, region_byte, 0, 0, 0, 0, 0]) + hashlib.sha1(table).digest()
    assert len(header) == 0x20
    data = SIGNATURE_MARKER + header + table + b''.join(blobs)
    open(out_path, 'wb').write(data)
    print(f'{out_path}: {len(data)} bytes; header SHA-1 {hashlib.sha1(header).hexdigest()}', file=sys.stderr)


if __name__ == '__main__':
    args = sys.argv[1:]
    region = 'normal'
    if len(args) >= 2 and args[0] == '--region':
        region = args[1]
        args = args[2:]
    if region not in REGIONS or len(args) not in (3, 4):
        sys.exit(__doc__)
    main(*args, region=region)
