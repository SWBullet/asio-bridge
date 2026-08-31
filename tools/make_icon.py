#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 ASIO Bridge 应用图标。

设计：暗色仪表面板圆角磁贴 + 琥珀色(#ffd27f)发光音频波形 + 桥接弧线/桥墩 + 输入/输出端点。
与 Web 控制台品牌一致（深色面板 + 琥珀强调色）。

输出：
  assets/icon.ico        Windows 多尺寸图标(256/128/64/48/32/24/16)
  assets/icon-256.png    256px 预览(透明底)

用法:
  python tools/make_icon.py
依赖: Pillow
"""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_ICO = os.path.join(ROOT, "assets", "icon.ico")
OUT_PNG = os.path.join(ROOT, "assets", "icon-256.png")

# ---- 调色板 ----
TILE_TOP = (0x33, 0x3A, 0x43)
TILE_BOT = (0x12, 0x15, 0x19)
EDGE = (0x6C, 0x75, 0x7F)
AMBER = (0xFF, 0xC2, 0x57)        # 主体琥珀
AMBER_HI = (0xFF, 0xE7, 0xB0)     # 高光
AMBER_GLOW = (0xFF, 0xD2, 0x7F)   # 辉光(品牌色)
AMBER_DIM = (0xA8, 0x6F, 0x25)    # 桥面/桥墩(暗琥珀)
NODE_RING = (0xFF, 0xF3, 0xDA)
NODE_CORE = (0x3A, 0x2A, 0x12)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def vgrad_tile(size, top, bot):
    """竖向渐变磁贴底(透明 alpha=255)。"""
    w, h = size
    img = Image.new("RGBA", size)
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = lerp(top, bot, t)
        for x in range(w):
            px[x, y] = (c[0], c[1], c[2], 255)
    return img


def rounded_rect_mask(size, rx):
    """返回圆形/圆角矩形 alpha 蒙版(1=内 0=外)。"""
    w, h = size
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=rx, fill=255)
    return m


def radial_glow(size, cx, cy, r, color, max_alpha):
    """径向辉光层(中心最亮，向外透明)。"""
    w, h = size
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    px = layer.load()
    r2 = r * r
    for y in range(h):
        for x in range(w):
            dx, dy = x - cx, y - cy
            d2 = dx * dx + dy * dy
            if d2 > r2:
                continue
            a = int(round(max_alpha * (1 - math.sqrt(d2) / r)))
            if a > 0:
                px[x, y] = (color[0], color[1], color[2], a)
    return layer


def cubic_bezier(p0, p1, p2, p3, n=160):
    """采样三次贝塞尔为点序列。"""
    pts = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        x = (u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0])
        y = (u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1])
        pts.append((x, y))
    return pts


def quadratic_bezier(p0, p1, p2, n=120):
    """采样二次贝塞尔为点序列。"""
    pts = []
    for i in range(n + 1):
        t = i / n
        u = 1 - t
        x = u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0]
        y = u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]
        pts.append((x, y))
    return pts


def render(size_px):
    S = size_px
    scale = S / 256.0
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    rx = int(round(50 * scale))
    # 磁贴底(渐变)
    tile = vgrad_tile((S, S), TILE_TOP, TILE_BOT)
    mask = rounded_rect_mask((S, S), rx)
    tile.putalpha(mask)
    img = Image.alpha_composite(img, tile)

    # 外描边(深色)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([1, 1, S - 2, S - 2], radius=rx, outline=(0x0A, 0x0C, 0x0F, 255), width=max(1, int(round(2 * scale))))
    # 顶部内高光(浅色斜角)
    d.rounded_rectangle([int(2 * scale), int(2 * scale), S - 3, S - 3], radius=rx - 1,
                        outline=(EDGE[0], EDGE[1], EDGE[2], 120), width=max(1, int(round(1.4 * scale))))

    # 暖色辉光
    glow = radial_glow((S, S), int(128 * scale), int(118 * scale), int(96 * scale), AMBER_GLOW, 150)
    glow = glow.filter(ImageFilter.GaussianBlur(int(round(3.2 * scale))))
    img = Image.alpha_composite(img, glow)

    # 桥面弧线 + 桥墩(暗琥珀)
    arc_w = max(2, int(round(6 * scale)))
    pier_w = max(2, int(round(12 * scale)))
    pier_h = max(2, int(round(18 * scale)))
    d = ImageDraw.Draw(img)
    bridge = [(x * scale, y * scale) for (x, y) in quadratic_bezier((52, 204), (128, 228), (204, 204))]
    d.line(bridge, fill=AMBER_DIM + (255,), width=arc_w, joint="curve")
    d.rounded_rectangle([44 * scale, 204 * scale, (44 + pier_w) * scale, (204 + pier_h) * scale],
                        radius=max(1, int(2 * scale)), fill=AMBER_DIM + (255,))
    d.rounded_rectangle([198 * scale, 204 * scale, (198 + pier_w) * scale, (204 + pier_h) * scale],
                        radius=max(1, int(2 * scale)), fill=AMBER_DIM + (255,))

    # 波形点(三段贝塞尔)
    p0 = (44, 132); p1 = (62, 86); p2 = (86, 86); p3 = (104, 132)
    p4 = (122, 178); p5 = (146, 178); p6 = (164, 132)
    p7 = (182, 86); p8 = (206, 86); p9 = (218, 132)
    wave = (cubic_bezier(p0, p1, p2, p3) + cubic_bezier(p3, p4, p5, p6)[1:] +
            cubic_bezier(p6, p7, p8, p9)[1:])
    wave = [(x * scale, y * scale) for (x, y) in wave]

    w_w = max(2, int(round(10 * scale)))
    # 波形辉光(模糊副本)
    glow_layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow_layer)
    gd.line(wave, fill=AMBER_GLOW + (255,), width=max(3, int(round(16 * scale))), joint="curve")
    glow_layer = glow_layer.filter(ImageFilter.GaussianBlur(int(round(3.6 * scale))))
    glow_layer.putalpha(glow_layer.split()[3].point(lambda a: int(a * 0.32)))
    img = Image.alpha_composite(img, glow_layer)

    # 波形主体 + 顶部高光
    d = ImageDraw.Draw(img)
    d.line(wave, fill=AMBER + (255,), width=w_w, joint="curve")
    hi = [(x, y - 2 * scale) for (x, y) in wave]
    d.line(hi, fill=AMBER_HI + (220,), width=max(1, int(round(3 * scale))), joint="curve")

    # 端点节点
    for (cx, cy) in [(44, 132), (218, 132)]:
        cx, cy = cx * scale, cy * scale
        r = max(3, int(round(12 * scale)))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=AMBER + (255,))
        rr = max(1, int(round(1.6 * scale)))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=NODE_RING + (217,), width=rr)
        rc = max(1, int(round(4.5 * scale)))
        d.ellipse([cx - rc, cy - rc, cx + rc, cy + rc], fill=NODE_CORE + (255,))

    return img


def main():
    master = render(256)
    master.save(OUT_PNG)

    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = []
    for s in sizes:
        if s == 256:
            frames.append(master)
        else:
            frames.append(master.resize((s, s), Image.LANCZOS))
    # Pillow ICO：每帧需为 RGBA 或 P；用 RGBA
    master.save(OUT_ICO, sizes=[(s, s) for s in sizes], optimize=True)
    print("wrote", OUT_ICO, "sizes", sizes)
    print("wrote", OUT_PNG)


if __name__ == "__main__":
    main()
