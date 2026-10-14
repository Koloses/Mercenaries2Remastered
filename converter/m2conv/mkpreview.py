# Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
# Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION).
"""DLC preview picture for the Extras > Downloadable Content details frame (270x335 px on screen)."""
import os
from PIL import Image, ImageFilter, ImageEnhance, ImageDraw, ImageFont, ImageOps
import cfg
A = cfg.w('art')
W, H = 540, 670
def make(out=None):
    t = Image.open(os.path.join(A, 'dlc_terrain.png')).convert('RGB')
    bg = t.crop((512, 1024, 512 + 768, 1024 + 952)).resize((W, H), Image.LANCZOS)
    bg = ImageEnhance.Color(bg).enhance(0.55); bg = ImageEnhance.Brightness(bg).enhance(0.55)
    bg = bg.filter(ImageFilter.GaussianBlur(2))
    # vignette
    v = Image.new('L', (W, H), 0); dv = ImageDraw.Draw(v)
    for i in range(60):
        dv.rectangle((i * 3, i * 4, W - i * 3, H - i * 4), fill=int(255 * (i / 60) ** 0.6))
    v = v.filter(ImageFilter.GaussianBlur(30))
    bg = Image.composite(bg, Image.new('RGB', (W, H), (8, 8, 6)), v)
    # skull emblem
    sk = Image.open(os.path.join(A, 'dlc_thumb.png')).convert('RGBA')
    sk = sk.resize((128, 128), Image.LANCZOS).resize((400, 400), Image.LANCZOS).filter(ImageFilter.UnsharpMask(3, 120, 2))
    m = Image.new('L', sk.size, 0); ImageDraw.Draw(m).ellipse((20, 20, 380, 380), fill=255)
    m = m.filter(ImageFilter.GaussianBlur(28))
    sk.putalpha(Image.composite(sk.getchannel('A'), Image.new('L', sk.size, 0), m))
    bg = bg.convert('RGBA'); bg.alpha_composite(sk, ((W - 400) // 2, 140))
    d = ImageDraw.Draw(bg)
    fb = os.path.join(cfg.DATA, 'DejaVuSansCondensed-Bold.ttf')
    def text(y, s, size, fill):
        f = ImageFont.truetype(fb, size); w = d.textlength(s, font=f)
        for dx, dy in ((3, 3), (2, 2)): d.text(((W - w) / 2 + dx, y + dy), s, font=f, fill=(0, 0, 0, 200))
        d.text(((W - w) / 2, y), s, font=f, fill=fill)
    text(40, 'BLOW IT UP', 78, (232, 196, 70, 255))
    text(512, "AGAIN", 88, (232, 196, 70, 255))
    text(616, 'FOUR NEW MISSIONS', 28, (220, 220, 210, 255))
    im = bg.convert('RGB')
    if out: im.save(out)
    return im
if __name__ == '__main__':
    import sys; make(sys.argv[1])
