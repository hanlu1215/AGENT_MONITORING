#!/usr/bin/env python3
"""生成程序图标：手绘卡通风格的「AI」手写字。

产物：
    res/app.ico    多尺寸图标（16/24/32/48/64/128/256），被 res/app.rc 编译进 exe
    res/logo.png   512px 的 PNG，README / 网页里想用可以用它

为什么不用现成图标：原来用的 openai-logo.png / app.ico 是 OpenAI 的注册商标，
放在公开仓库里（而且会被嵌进 exe）有法律风险，所以换成自绘的。

画法：超椭圆（squircle）背景 + 手工扰动让边缘不那么规整，
再用 Comic Sans MS Bold 写「AI」，加描边和一点点歪斜，做出手写卡通的感觉。

    .\\.venv\\Scripts\\python.exe tools\\make_icon.py
"""

from __future__ import annotations

import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

APP_DIR = Path(__file__).resolve().parents[1]
RES_DIR = APP_DIR / "res"

SIZES = (16, 24, 32, 48, 64, 128, 256)
SUPERSAMPLE = 4                      # 先画大图再缩小，边缘才平滑

BG_TOP = (139, 123, 255)             # 淡紫
BG_BOTTOM = (74, 54, 178)            # 深紫
INK = (36, 28, 77)                   # 手绘描边用的深色
LETTER = (255, 253, 246)             # 米白，像纸上的白墨
SPARK = (255, 224, 102)              # 暖黄点缀

FONT_CANDIDATES = (
    r"C:\Windows\Fonts\comicbd.ttf",   # Comic Sans MS Bold：卡通手写感最足
    r"C:\Windows\Fonts\comic.ttf",
    r"C:\Windows\Fonts\segoeuib.ttf",  # 没有 Comic Sans 时的退路
    r"C:\Windows\Fonts\arialbd.ttf",
)


def load_font(px: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return ImageFont.truetype(path, px)
    raise SystemExit("找不到可用字体，请检查上面的 FONT_CANDIDATES")


def squircle_points(cx: float, cy: float, radius: float, n: float, steps: int,
                    wobble: float, rng: random.Random):
    """超椭圆的轮廓点；wobble>0 时加一点不规则，模拟手画的抖动。"""
    phase1, phase2 = rng.uniform(0, 6.28), rng.uniform(0, 6.28)
    points = []
    for i in range(steps):
        theta = 2 * math.pi * i / steps
        cos_t, sin_t = math.cos(theta), math.sin(theta)
        # |x|^n + |y|^n = 1 的参数化
        denom = (abs(cos_t) ** n + abs(sin_t) ** n) ** (1.0 / n)
        r = radius / denom
        if wobble:
            r *= 1.0 + wobble * math.sin(7 * theta + phase1) \
                 + wobble * 0.6 * math.sin(11 * theta + phase2)
        points.append((cx + r * cos_t, cy + r * sin_t))
    return points


def diagonal_gradient(size: int, top, bottom) -> Image.Image:
    img = Image.new("RGBA", (size, size))
    px = img.load()
    for y in range(size):
        t = y / max(1, size - 1)
        base = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(size):
            # 再按横向略作明暗，避免死板
            k = 1.0 + 0.06 * (1 - x / max(1, size - 1))
            px[x, y] = (min(255, int(base[0] * k)), min(255, int(base[1] * k)),
                        min(255, int(base[2] * k)), 255)
    return img


def draw_sparkle(draw: ImageDraw.ImageDraw, cx: float, cy: float, r: float, color):
    """四角星：两条交叉的细菱形，卡通点缀用。"""
    w = r * 0.26
    draw.polygon([(cx, cy - r), (cx + w, cy), (cx, cy + r), (cx - w, cy)], fill=color)
    draw.polygon([(cx - r, cy), (cx, cy + w), (cx + r, cy), (cx, cy - w)], fill=color)


def render(size: int) -> Image.Image:
    n = size * SUPERSAMPLE
    rng = random.Random(20261013)          # 固定种子：每次生成的抖动一致
    canvas = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    # ---- 底：手绘超椭圆 ----
    cx = cy = n / 2
    radius = n * 0.47
    outline = squircle_points(cx, cy, radius, 3.6, 320, 0.010, rng)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).polygon(outline, fill=255)
    canvas.paste(diagonal_gradient(n, BG_TOP, BG_BOTTOM), (0, 0), mask)

    # ---- 手绘描边：沿轮廓再画一圈，带同样的抖动 ----
    stroke = ImageDraw.Draw(canvas)
    width = max(2, int(n * 0.022))
    stroke.line(outline + outline[:1], fill=INK + (255,), width=width, joint="curve")

    # ---- 字母「AI」 ----
    font_target = n * 0.30                # 先给个字号，再按实际宽度回调
    font = load_font(int(font_target))
    text = "AI"
    box = font.getbbox(text, stroke_width=int(font.size * 0.06))
    text_w, text_h = box[2] - box[0], box[3] - box[1]
    scale = (n * 0.62) / max(1, text_w)   # 让字占宽度的六成左右
    font = load_font(max(8, int(font.size * scale)))

    layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    pen = ImageDraw.Draw(layer)
    outline_w = max(1, int(font.size * 0.075))
    pen.text((n / 2, n / 2), text, font=font, fill=LETTER + (255,),
             anchor="mm", stroke_width=outline_w, stroke_fill=INK + (255,))

    layer = layer.rotate(-5, resample=Image.BICUBIC, center=(n / 2, n / 2))
    canvas.alpha_composite(layer)

    # ---- 卡通点缀 ----
    if size >= 32:
        draw_sparkle(stroke, n * 0.765, n * 0.235, n * 0.075, SPARK + (255,))
        draw_sparkle(stroke, n * 0.845, n * 0.360, n * 0.042, SPARK + (230,))

    return canvas.resize((size, size), Image.LANCZOS)


def main() -> int:
    RES_DIR.mkdir(parents=True, exist_ok=True)
    frames = [render(s) for s in SIZES]

    ico = RES_DIR / "app.ico"
    frames[-1].save(ico, format="ICO", sizes=[(s, s) for s in SIZES])
    # Pillow 的 ICO 只能存一个尺寸清单，用 append_images 保证每个尺寸都是原生渲染的
    frames[-1].save(ico, format="ICO", append_images=frames[:-1],
                    sizes=[(s, s) for s in SIZES])

    logo = RES_DIR / "logo.png"
    render(512).save(logo)

    print(f"已生成 {ico}（{', '.join(str(s) for s in SIZES)}）")
    print(f"已生成 {logo}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
