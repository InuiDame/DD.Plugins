#!/usr/bin/env python3
"""按原版等级顺序生成 resolve_level_bar 的等级徽章图。

原版规律（以 Darkest.exe 内置资源实测）：
  lvl0/1  灰色中心        hue≈43  S≈0.05 L≈0.25-0.30
  lvl2    棕褐            hue≈40  S≈0.17 L≈0.58
  lvl3    绿色            hue≈107 S≈0.30 L≈0.43
  lvl4    蓝色            hue≈217 S≈0.47 L≈0.45
  lvl5    橙              hue≈29  S≈1.00 L≈0.30
  lvl6    红              hue≈356 S≈1.00 L≈0.30
5 级起外框变成同款华丽金框，只有中心宝石换色，所以 7 级以上继续沿用这个规律：
只改宝石色相/饱和/亮度，金色外框保持不动。

用法：
  python gen_level_icons.py --game "<游戏根目录>" --out <输出目录> [--from 7] [--to 10]
"""
from __future__ import annotations

import argparse
import colorsys
import os
import shutil

from PIL import Image

# 7 级起在“橙→红→紫”之后继续往下走，选择区分度高的高段配色。
RAMP = {
    7: (285.0, 1.00, 0.30),   # 紫（与原版社区常用 7 级配色一致）
    8: (190.0, 1.00, 0.30),   # 青
    9: (325.0, 1.00, 0.30),   # 品红
    10: (210.0, 0.12, 0.90),  # 冷白（最高级）
}

TEMPLATE_BACKGROUND = "resolve_level_bar_number_background_lvl6.png"
TEMPLATE_GLOW = "resolve_level_bar_number_background_glow_lvl6.png"


def recolor_gem(src: Image.Image, hue: float, sat: float, light: float) -> Image.Image:
    """只替换中心宝石（高饱和的红/紫像素），金框与小件装饰原样保留。"""
    out = Image.new("RGBA", src.size)
    src_px = src.load()
    out_px = out.load()
    for y in range(src.height):
        for x in range(src.width):
            r, g, b, a = src_px[x, y]
            if a == 0:
                out_px[x, y] = (0, 0, 0, 0)
                continue
            h, l, s = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
            hd = h * 360.0
            is_gem = s >= 0.55 and (hd >= 330.0 or hd <= 15.0)
            if not is_gem:
                out_px[x, y] = (r, g, b, a)
                continue
            # 保留原有的明暗层次：以模板宝石基准亮度 0.30 做比例缩放
            brightness = l / 0.30
            nl = min(1.0, max(0.0, light * brightness))
            nr, ng, nb = colorsys.hls_to_rgb(hue / 360.0, nl, sat)
            out_px[x, y] = (int(nr * 255), int(ng * 255), int(nb * 255), a)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="游戏根目录")
    ap.add_argument("--out", required=True, help="输出目录（放 *.png）")
    ap.add_argument("--from", dest="lo", type=int, default=7)
    ap.add_argument("--to", dest="hi", type=int, default=10)
    args = ap.parse_args()

    src_dir = os.path.join(args.game, "shared", "resolve_level_bar")
    bg_path = os.path.join(src_dir, TEMPLATE_BACKGROUND)
    glow_path = os.path.join(src_dir, TEMPLATE_GLOW)
    if not os.path.isfile(bg_path):
        raise SystemExit("找不到模板：" + bg_path)

    os.makedirs(args.out, exist_ok=True)
    template = Image.open(bg_path).convert("RGBA")

    for level in range(args.lo, args.hi + 1):
        if level not in RAMP:
            raise SystemExit("未定义 %d 级配色，请在 RAMP 中补充" % level)
        hue, sat, light = RAMP[level]
        img = recolor_gem(template, hue, sat, light)
        dst = os.path.join(args.out, "resolve_level_bar_number_background_lvl%d.png" % level)
        img.save(dst, optimize=True)

        # 原版 4/5/6 级的 glow 是全透明（931 字节那种），高段保持一致
        glow_dst = os.path.join(
            args.out, "resolve_level_bar_number_background_glow_lvl%d.png" % level
        )
        if os.path.isfile(glow_path):
            shutil.copyfile(glow_path, glow_dst)
        else:
            Image.new("RGBA", template.size, (0, 0, 0, 0)).save(glow_dst, optimize=True)

        print("generated lvl%d hue=%.0f sat=%.2f light=%.2f -> %s" % (level, hue, sat, light, dst))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
