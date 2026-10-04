"""Draws the 知意输入法 (Zhiyi IME) icons.

  zhiyi.ico     app / installer icon: blue rounded square, white "知", gold four-point sparkle
                (the same mark the candidate window puts on the Laya recommendation)
  icon_zh.ico   language bar / tray: 中      icon_en.ico: 英      icon_c.ico: A (Caps Lock)

  python resource/make_icons.py      (needs Pillow and the Microsoft YaHei / Segoe UI fonts)
"""
import math
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = "C:/Windows/Fonts"
BLUE_TOP = (52, 140, 255)
BLUE_BOTTOM = (18, 92, 214)
BRAND = (24, 110, 235)
GOLD = (255, 196, 46)
APP_SIZES = [256, 128, 96, 72, 64, 48, 32, 24, 16]
MODE_SIZES = [64, 48, 40, 32, 24, 20, 16]
SS = 4  # supersampling


def star(draw, cx, cy, r, fill, inner=0.3):
    pts = []
    for i in range(8):
        a = -math.pi / 2 + i * math.pi / 4
        rr = r if i % 2 == 0 else r * inner
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    draw.polygon(pts, fill=fill)


def centered_text(draw, box, text, font, fill):
    x0, y0, x1, y1 = box
    l, t, r, b = draw.textbbox((0, 0), text, font=font)
    draw.text(((x0 + x1 - (r - l)) / 2 - l, (y0 + y1 - (b - t)) / 2 - t), text, font=font, fill=fill)


def app_icon(size):
    n = size * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    # vertical gradient clipped to a rounded square
    grad = Image.new("RGBA", (n, n))
    gd = ImageDraw.Draw(grad)
    for y in range(n):
        t = y / max(1, n - 1)
        c = tuple(int(BLUE_TOP[i] + (BLUE_BOTTOM[i] - BLUE_TOP[i]) * t) for i in range(3))
        gd.line([(0, y), (n, y)], fill=c + (255,))
    mask = Image.new("L", (n, n), 0)
    pad = n * 0.04
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, n - pad, n - pad], radius=n * 0.22, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    small = size <= 24
    # "知": slightly lower-left so the sparkle has room at the top-right
    font = ImageFont.truetype(os.path.join(FONTS, "msyhbd.ttc"), int(n * (0.62 if small else 0.54)))
    off = n * (0.03 if small else 0.06)
    centered_text(d, (pad - off, pad + off, n - pad - off, n - pad + off), "知", font, (255, 255, 255, 255))
    # sparkle inside the tile's top-right corner (and a small companion below it when there is room)
    r = n * (0.14 if small else 0.12)
    cx, cy = n - pad - r * 1.45, pad + r * 1.45
    star(d, cx, cy, r, GOLD + (255,))
    if size >= 48:
        star(d, cx + r * 0.25, cy + r * 1.75, r * 0.42, GOLD + (235,))
    return img.resize((size, size), Image.LANCZOS)


def mode_icon(size, text, font_file, scale):
    n = size * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(os.path.join(FONTS, font_file), int(n * scale))
    centered_text(d, (0, 0, n, n), text, font, BRAND + (255,))
    return img.resize((size, size), Image.LANCZOS)


def save_ico(path, images):
    images = sorted(images, key=lambda im: -im.width)
    images[0].save(path, format="ICO", sizes=[(im.width, im.height) for im in images],
                   append_images=images[1:])


def main():
    save_ico(os.path.join(HERE, "zhiyi.ico"), [app_icon(s) for s in APP_SIZES])
    for name, text, font, scale in (("icon_zh.ico", "中", "msyhbd.ttc", 0.92),
                                    ("icon_en.ico", "英", "msyhbd.ttc", 0.92),
                                    ("icon_c.ico", "A", "segoeuib.ttf", 0.95)):
        save_ico(os.path.join(HERE, name), [mode_icon(s, text, font, scale) for s in MODE_SIZES])
    print("icons written")


if __name__ == "__main__":
    main()
