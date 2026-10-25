#!/usr/bin/env python3
"""Make the Vita LiveArea pictures from the game's own art (run by CMake for the Vita build).

The loading screen's logo (textures FFloadscreen00#0 and #1 of
Scenes/FF-StartUp.ZIP -> SCENES/FF-StartUp.TEX, two 256x256 halves of a 512x256
picture: the molotov emblem and "FREEDOM FIGHTERS" over a blue glow on black)
becomes:
  sce_sys/icon0.png                       128x128  bubble icon (the emblem and title)
  sce_sys/pic0.png                        960x544  shown while the app starts
  sce_sys/livearea/contents/bg.png        840x500  LiveArea background, with the credits
  sce_sys/livearea/contents/startup.png   280x158  LiveArea start gate (logo)
All are 8-bit palette PNGs, as the LiveArea expects; pic0.png is also what
platform_vita.c shows while the game starts.

The pictures are derived from the retail data, so they are not kept in the
repo: CMake writes them into <build>/sce_sys next to a copy of
platform/vita/sce_sys (the LiveArea template).

With the key art in <game directory>/../art (keyart.jpg, 16:9, the logo in its
upper middle; icon.jpg, square), not in the repo either, the background, the
start-up picture and the gate come from keyart.jpg and the icon from icon.jpg.

usage: tools/make_vita_livearea.py [game directory] [sce_sys directory] [port version]
       (defaults: gamedata/pc, build-vita/sce_sys, no version on the LiveArea)
Needs Pillow. The credits use Oswald (platform/vita/fonts, SIL Open Font License).

TEX archive (Glacier): u32 offsets of two index tables, two more words, then
entries of { u32 size (of the whole entry), char type[4] (reversed: "NLAP" is
PALN), char type2[4], u32 index, u16 height, u16 width, u32 mip count, 3 words,
name\\0, per mip { u32 bytes, pixels }, PALN: { u32 count, BGRA[count] } }.
"""
import os
import struct
import sys
import zipfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, 'platform', 'vita', 'fonts', 'Oswald.ttf')

# At the bottom of the LiveArea, so everyone who opens the bubble knows what this is.
# CMake passes no version: an update made from the game replaces its files but not
# sce_sys, so a version here would go stale (the boot screen shows the current one).
CREDITS = [
    'Freedom Fighters for PS Vita  -  PS Vita port by HenryKun55, made with love',
    'Freedom Fighters (c) IO Interactive / Electronic Arts. Not affiliated with them or Sony.',
    'Needs your own copy of the game.',
]


def tex_textures(data, wanted):
    """{name: RGBA image} of the PALN textures named in `wanted` from a TEX archive."""
    out = {}
    end, = struct.unpack_from('<I', data, 0)
    pos = 16
    while pos + 0x24 < end:
        size, t1, _t2, _idx, h, w, _mips = struct.unpack_from('<I4s4sIHHI', data, pos)
        if size < 0x24 or pos + size > len(data):
            break
        ne = data.index(b'\0', pos + 0x24)
        name = data[pos + 0x24:ne].decode('latin-1')
        if name in wanted and t1 == b'NLAP':
            q = ne + 1
            n, = struct.unpack_from('<I', data, q)
            pixels = data[q + 4:q + 4 + n]
            q += 4 + n
            count, = struct.unpack_from('<I', data, q)
            pal = bytearray(256 * 4)
            for i in range(min(count, 256)):
                b, g, r, a = data[q + 4 + 4 * i:q + 8 + 4 * i]
                pal[4 * i:4 * i + 4] = bytes((r, g, b, a))
            rgba = bytearray(w * h * 4)
            for i, p in enumerate(pixels[:w * h]):
                rgba[4 * i:4 * i + 4] = pal[4 * p:4 * p + 4]
            out[name] = Image.frombytes('RGBA', (w, h), bytes(rgba))
        pos += size
    missing = set(wanted) - set(out)
    if missing:
        raise ValueError('textures not found: %s' % ', '.join(sorted(missing)))
    return out


def load_logo(game):
    with zipfile.ZipFile(os.path.join(game, 'Scenes', 'FF-StartUp.ZIP')) as z:
        name = next(n for n in z.namelist() if n.upper().endswith('FF-STARTUP.TEX'))
        tex = tex_textures(z.read(name), {'FFloadscreen00#0', 'FFloadscreen00#1'})
    logo = Image.new('RGB', (512, 256))
    logo.paste(tex['FFloadscreen00#0'].convert('RGB'), (0, 0))
    logo.paste(tex['FFloadscreen00#1'].convert('RGB'), (256, 0))
    return logo


def content_box(img, threshold=40):
    """Bounding box of the pixels brighter than `threshold` (the logo without its black)."""
    mask = img.convert('L').point(lambda v: 255 if v > threshold else 0)
    return mask.getbbox() or (0, 0, img.width, img.height)


def fit(img, size, background=(0, 0, 0), scale=1.0):
    """img scaled to fit size (aspect kept, times `scale`), centered on a background."""
    w, h = size
    s = min(w / img.width, h / img.height) * scale
    scaled = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    out = Image.new('RGB', size, background)
    out.paste(scaled, ((w - scaled.width) // 2, (h - scaled.height) // 2))
    return out


def draw_credits(img, version):
    """The credit lines, centred at the bottom over a band that darkens from above."""
    lines = list(CREDITS)
    if version:
        lines[-1] += '  v' + version
    font = ImageFont.truetype(FONT, 16)
    font.set_variation_by_name('Regular')
    img = img.convert('RGBA')
    w, h = img.size
    line, top = 21, h - 21 * len(lines) - 10
    band = Image.new('L', (w, h), 0)
    bd = ImageDraw.Draw(band)
    for y in range(top - 24, h):
        bd.line([(0, y), (w, y)], fill=min(190, max(0, (y - (top - 24)) * 190 // 24)))
    img.paste(Image.new('RGBA', (w, h), (0, 0, 0, 255)), (0, 0), band)
    d = ImageDraw.Draw(img)
    for i, text in enumerate(lines):
        tw = d.textlength(text, font=font)
        x, y = (w - tw) / 2, top + i * line
        d.text((x + 1, y + 1), text, font=font, fill=(0, 0, 0, 255))
        d.text((x, y), text, font=font, fill=(255, 205, 120, 255) if i == 0 else (225, 225, 225, 255))
    return img.convert('RGB')


def save_png8(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)
    print('wrote %s (%dx%d)' % (path, img.width, img.height))


def cover(img, size):
    """img scaled to cover size (aspect kept), cropped to it around the center."""
    w, h = size
    s = max(w / img.width, h / img.height)
    scaled = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x, y = (scaled.width - w) // 2, (scaled.height - h) // 2
    return scaled.crop((x, y, x + w, y + h))


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'gamedata', 'pc')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'build-vita', 'sce_sys')
    version = sys.argv[3] if len(sys.argv) > 3 else ''
    art_dir = os.path.join(os.path.dirname(os.path.abspath(game)), 'art')
    keyart_path = os.path.join(art_dir, 'keyart.jpg')
    icon_path = os.path.join(art_dir, 'icon.jpg')
    if os.path.exists(keyart_path):
        key = Image.open(keyart_path).convert('RGB')
        boot = cover(key, (960, 544))
        save_png8(boot, os.path.join(out, 'pic0.png'))
        # The credits go over the city skyline at the bottom.
        save_png8(draw_credits(cover(key, (840, 500)), version), os.path.join(out, 'livearea', 'contents', 'bg.png'))
        # The gate: the logo, cut from the upper middle of the key art.
        w, h = key.size
        gw = w * 0.62
        gh = gw * 158 / 280
        cx, cy = w * 0.5, h * 0.36
        gate = key.crop((round(cx - gw / 2), round(cy - gh / 2), round(cx + gw / 2), round(cy + gh / 2)))
        save_png8(gate.resize((280, 158), Image.LANCZOS), os.path.join(out, 'livearea', 'contents', 'startup.png'))
    else:
        logo = load_logo(game)
        # The logo with its glow, without the empty black around it.
        l, t, r, b = content_box(logo, 12)
        art = logo.crop((max(0, l - 16), max(0, t - 16), min(512, r + 16), min(256, b + 16)))
        boot = fit(art, (960, 544), scale=0.8)
        save_png8(boot, os.path.join(out, 'pic0.png'))
        bg = Image.new('RGB', (840, 500))
        bg.paste(fit(art, (840, 420), scale=0.85), (0, 0))
        save_png8(draw_credits(bg, version), os.path.join(out, 'livearea', 'contents', 'bg.png'))
        save_png8(fit(art, (280, 158), scale=0.95), os.path.join(out, 'livearea', 'contents', 'startup.png'))
    if os.path.exists(icon_path):
        icon = cover(Image.open(icon_path).convert('RGB'), (128, 128))
    else:
        logo = load_logo(game)
        l, t, r, b = content_box(logo, 60)
        side = max(r - l, b - t) + 24
        cx, cy = (l + r) // 2, (t + b) // 2
        icon = logo.crop((cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)).resize((128, 128), Image.LANCZOS)
    save_png8(icon, os.path.join(out, 'icon0.png'))


if __name__ == '__main__':
    main()
