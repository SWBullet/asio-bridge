#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 ASIO Bridge 应用图标。

设计：暗色仪表面板圆角磁贴 + 琥珀色(#ffd27f)发光 + 声波柱(9 根圆头竖条)。
柱形比例取自 Icons8 audio-wave 实测（M 形波形：外中-次高-中心矮-次高-外中）。
与 Web 控制台品牌一致（深色面板 + 琥珀强调色）。

按尺寸分级渲染，避免小尺寸糊成一团：
  >= 96px : 9 根柱（完整形）
  40~64px : 5 根柱
  <  40px : 3 根柱（放大间距与柱宽）

输出：
  assets/icon.ico        Windows 多尺寸图标(256/128/96/64/48/40/32/24/20/16)
  assets/icon-256.png    256px 预览
  assets/icon.svg        矢量设计源
  build/_icon/preview.png 深/浅背景验收图

用法:
  python tools/make_icon.py
依赖: Pillow
"""
import math
import os
import struct

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_ICO = os.path.join(ROOT, "assets", "icon.ico")
OUT_PNG = os.path.join(ROOT, "assets", "icon-256.png")
OUT_SVG = os.path.join(ROOT, "assets", "icon.svg")
OUT_PREVIEW = os.path.join(ROOT, "build", "_icon", "preview.png")

# ---- 调色板 ----
TILE_TOP = (0x33, 0x3A, 0x43)
TILE_BOT = (0x12, 0x15, 0x19)
EDGE = (0x6C, 0x75, 0x7F)
AMBER = (0xFF, 0xC2, 0x57)        # 主体琥珀
AMBER_HI = (0xFF, 0xE7, 0xB0)     # 高光
AMBER_GLOW = (0xFF, 0xD2, 0x7F)   # 辉光(品牌色)
AMBER_DIM = (0xA8, 0x6F, 0x25)    # 底部暗琥珀

# ---- 设计参考系：所有几何按 256px 定义，渲染时乘 scale ----
REF = 256.0
BAR_PEAK = 143.0 / REF            # 最高柱占画布高比例(0.5586)
BAR_BASE = 187.7                  # bars 值 → 像素高的换算(143 / 0.7617)

# 9 根柱：Icons8 audio-wave 实测高度(相对画布)
BARS9 = [0.1602, 0.4023, 0.7617, 0.3594, 0.1602, 0.3594, 0.7617, 0.4023, 0.1602]
# 5 根：抽样自 9 根，保留 M 形轮廓
BARS5 = [BARS9[1], BARS9[2], BARS9[4], BARS9[6], BARS9[7]]
# 3 根：中心抬高，否则小尺寸下中间柱细到看不见
BARS3 = [0.45, 0.90, 0.45]

SPAN = 184.0 / REF                # 柱区横向占用(36..220)
BAR_W = {9: 7.5 / REF, 5: 15.1 / REF, 3: 23.0 / REF}

LAYOUT_STEPS = ((96, 9), (40, 5), (0, 3))   # (最低尺寸, 柱数)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def vgrad(size, top, bot):
    """竖向渐变（1px 宽生成后拉伸，避免逐像素循环）"""
    w, h = size
    col = Image.new("RGB", (1, h))
    px = col.load()
    for y in range(h):
        px[0, y] = lerp(top, bot, y / (h - 1))
    return col.resize((w, h), Image.NEAREST)


def radial_glow_mask(size, cx, cy, r, max_alpha, res=192):
    """径向辉光遮罩（低分辨率计算后放大，平滑且快）"""
    m = Image.new("L", (res, res), 0)
    px = m.load()
    scx, scy, sr = cx * res / size[0], cy * res / size[1], r * res / size[0]
    sr2 = sr * sr
    for y in range(res):
        for x in range(res):
            dx, dy = x - scx, y - scy
            d2 = dx * dx + dy * dy
            if d2 <= sr2:
                px[x, y] = int(max_alpha * (1 - math.sqrt(d2) / sr))
    return m.resize(size, Image.LANCZOS)


def layout_for(px):
    for min_px, n in LAYOUT_STEPS:
        if px >= min_px:
            return n
    return 3


def bar_geometry(px, n):
    """返回 (柱宽像素, [(中心x, 柱高)] 列表)，含最小宽度兜底

    注意 pitch 要用 (跨度 - 柱宽)/(柱数-1)：跨度含柱宽，否则柱区右侧会溢出、
    左右边距不对称。
    """
    bars = {9: BARS9, 5: BARS5, 3: BARS3}[n]
    w = max(1.0, BAR_W[n] * px)
    span = SPAN * px
    pitch = (span - w) / max(1, n - 1) if n > 1 else 0.0
    gold_k = BAR_PEAK * px / max(bars)
    cx0 = (px - span) / 2.0 + w / 2.0
    out = []
    for i, b in enumerate(bars):
        out.append((cx0 + i * pitch, b * gold_k))
    return w, out


def render(px):
    """渲染单尺寸图标（内部按 px 直接绘制；调用方负责选合适母版）"""
    n = layout_for(px)
    S = px
    scale = S / REF
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # 圆角磁贴底
    rx = int(round(50 * scale))
    tile = vgrad((S, S), TILE_TOP, TILE_BOT).convert("RGBA")
    tile.putalpha(_rr_mask(S, rx))
    img = Image.alpha_composite(img, tile)

    d = ImageDraw.Draw(img)
    # 外描边 + 顶部内高光
    d.rounded_rectangle([1, 1, S - 2, S - 2], radius=rx,
                        outline=(0x0A, 0x0C, 0x0F, 255), width=max(1, int(round(2 * scale))))
    d.rounded_rectangle([int(2 * scale), int(2 * scale), S - 3, S - 3], radius=max(1, rx - 1),
                        outline=(EDGE[0], EDGE[1], EDGE[2], 120), width=max(1, int(round(1.4 * scale))))

    # 暖色辉光
    glow = Image.new("RGBA", (S, S), AMBER_GLOW + (0,))
    glow.putalpha(radial_glow_mask((S, S), int(128 * scale), int(118 * scale),
                                   int(96 * scale), 150))
    glow = glow.filter(ImageFilter.GaussianBlur(max(1, int(round(3.2 * scale)))))
    img = Image.alpha_composite(img, glow)

    # 柱几何
    bw, bars = bar_geometry(S, n)
    cy = S * 0.5

    # 柱形遮罩（圆头 = 圆角半径取半宽）
    bar_mask = Image.new("L", (S, S), 0)
    bd = ImageDraw.Draw(bar_mask)
    for (cx, h) in bars:
        bd.rounded_rectangle([cx - bw / 2, cy - h / 2, cx + bw / 2, cy + h / 2],
                             radius=bw / 2, fill=255)

    # 柱辉光（模糊副本）
    g = Image.new("RGBA", (S, S), AMBER_GLOW + (0,))
    gm = bar_mask.filter(ImageFilter.GaussianBlur(max(1, int(round(4.0 * scale)))))
    g.putalpha(gm.point(lambda a: int(a * 0.42)))
    img = Image.alpha_composite(img, g)

    # 柱主体：竖向渐变（上亮下暗，呼应上方光源）
    grad = vgrad((S, S), AMBER_HI, AMBER_DIM).convert("RGBA")
    img.paste(grad, (0, 0), bar_mask)

    # 柱顶部加一道更亮的高光（仅中尺寸以上，小尺寸会糊）
    if px >= 40:
        hi_mask = Image.new("L", (S, S), 0)
        hd = ImageDraw.Draw(hi_mask)
        for (cx, h) in bars:
            top = cy - h / 2
            hd.rounded_rectangle([cx - bw / 2, top, cx + bw / 2, top + max(1.0, h * 0.30)],
                                 radius=bw / 2, fill=90)
        img.paste(Image.new("RGBA", (S, S), AMBER_HI + (255,)), (0, 0), hi_mask)

    return img


def _rr_mask(size, rx):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius=rx, fill=255)
    return m


# 分级母版：每组渲染一次母版再降采样，成本低且小尺寸不糊
GROUPS = ((9, 1024, [256, 128, 96]), (5, 512, [64, 48, 40]), (3, 192, [32, 24, 20, 16]))


def build_images():
    images = {}
    for n, master_px, targets in GROUPS:
        # render() 内部按 px 选 layout，这里强制按组柱数：临时替换阈值
        m = _render_with(px=master_px, n=n)
        images[master_px] = m
        for t in targets:
            images[t] = m.resize((t, t), Image.LANCZOS)
    return images


def _render_with(px, n):
    """按指定柱数渲染（绕过 layout_for 的尺寸推断）"""
    global LAYOUT_STEPS
    saved = LAYOUT_STEPS
    LAYOUT_STEPS = ((0, n),)
    try:
        return render(px)
    finally:
        LAYOUT_STEPS = saved


def write_ico(images, sizes, path):
    """手工组装 ICO（PNG-in-ICO，Win7+ 支持），每尺寸用各自优化过的位图"""
    entries, blobs = [], []
    offset = 6 + 16 * len(sizes)
    for s in sizes:
        tmp = os.path.join(os.path.dirname(path), "_ico_tmp_%d.png" % s)
        images[s].save(tmp, format="PNG", optimize=True)
        data = open(tmp, "rb").read()
        os.remove(tmp)
        blobs.append(data)
        entries.append(struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0,
                                   0, 0, 1, 32, len(data), offset))
        offset += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(sizes)))
        for e in entries:
            f.write(e)
        for b in blobs:
            f.write(b)


def write_svg(path):
    """矢量设计源（与位图同构，便于日后改尺寸/改色）"""
    px = 256.0
    bw, bars = bar_geometry(px, 9)
    cy = px * 0.5
    lines = []
    for (cx, h) in bars:
        if h < 1:
            continue
        lines.append('  <line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" '
                     'stroke="url(#gold)" stroke-width="%.2f" stroke-linecap="round"/>'
                     % (cx, cy - h / 2, cx, cy + h / 2, bw))
    svg = """<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <defs>
    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#333a43"/><stop offset="1" stop-color="#121519"/>
    </linearGradient>
    <radialGradient id="glow" cx="0.5" cy="0.46" r="0.375">
      <stop offset="0" stop-color="#ffd27f" stop-opacity="0.59"/>
      <stop offset="1" stop-color="#ffd27f" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="gold" x1="0" y1="0.2" x2="0" y2="0.8">
      <stop offset="0" stop-color="#ffe7b0"/><stop offset="1" stop-color="#a86f25"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="256" height="256" rx="50" fill="url(#tile)"/>
  <rect x="0" y="0" width="256" height="256" rx="50" fill="url(#glow)"/>
  <rect x="2" y="2" width="252" height="252" rx="48" fill="none"
        stroke="#6c757f" stroke-opacity="0.47" stroke-width="1.4"/>
%s
</svg>
""" % ("\n".join(lines))
    open(path, "w", encoding="utf-8", newline="\n").write(svg)


def write_preview(images, path):
    """深/浅背景对照，验收各尺寸可见性"""
    order = [256, 128, 96, 64, 48, 40, 32, 24, 20, 16]
    pad, gap, label_h = 22, 18, 20
    rowh = max(order) + label_h + 8
    W = pad * 2 + sum(order) + gap * (len(order) - 1)
    H = pad * 2 + rowh * 2 + 30
    pv = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(pv)
    y = pad
    for bg, fg in (((28, 28, 32), (170, 170, 175)), ((242, 242, 247), (110, 110, 115))):
        d.rectangle([0, y - 6, W, y + rowh], fill=bg)
        x = pad
        base = y + label_h
        for s in order:
            im = images[s]
            pv.paste(im, (x, base + (max(order) - s) // 2), im)
            d.text((x, y + 2), str(s), fill=fg)
            x += s + gap
        y += rowh + 30
    pv.save(path, format="PNG", optimize=True)


def main():
    images = build_images()
    write_ico(images, [16, 20, 24, 32, 40, 48, 64, 96, 128, 256], OUT_ICO)
    images[256].save(OUT_PNG, format="PNG", optimize=True)
    write_svg(OUT_SVG)
    os.makedirs(os.path.dirname(OUT_PREVIEW), exist_ok=True)
    write_preview(images, OUT_PREVIEW)
    for p in (OUT_ICO, OUT_PNG, OUT_SVG, OUT_PREVIEW):
        print("%-38s %8d bytes" % (os.path.relpath(p, ROOT), os.path.getsize(p)))
    # 各尺寸柱数一览
    print("layout:", ", ".join("%dpx->%dbar" % (s, layout_for(s))
                               for s in (256, 96, 64, 40, 32, 16)))


if __name__ == "__main__":
    main()
