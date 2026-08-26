#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Acrylic Plate CAD Preview - matplotlib renderer (clean, white, English labels).
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager
# 让 matplotlib 优先用系统中文字体
font_manager.fontManager.addfont() if False else None
plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'Noto Sans SC',
                                   'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False
from matplotlib.patches import Rectangle, Circle, FancyArrowPatch
import numpy as np

PLATE_THICKNESS = 3.0

TOP = dict(
    name="Top Plate",
    W=80.0, H=55.0,
    inner_long=57.15, inner_short=27.94,
    inner_d=2.2, outer_d=3.2,
    outer_margin=3.0,
    title_color="#1d4ed8",
)
BOT = dict(
    name="Bottom Plate",
    W=80.0, H=55.0,
    inner_long=44.32, inner_short=22.00,
    inner_d=2.2, outer_d=3.2,
    outer_margin=3.0,
    title_color="#7c3aed",
)


def inner_xy(cfg):
    dx = (cfg['W'] - cfg['inner_long']) / 2
    dy = (cfg['H'] - cfg['inner_short']) / 2
    return [(dx, dy),
            (cfg['W']-dx, dy),
            (dx, cfg['H']-dy),
            (cfg['W']-dx, cfg['H']-dy)]


def outer_xy(cfg):
    m = cfg['outer_margin']
    return [(m, m), (cfg['W']-m, m), (m, cfg['H']-m), (cfg['W']-m, cfg['H']-m)]


def draw_h_dim(ax, x1, x2, y, text, color='#1d4ed8'):
    """水平尺寸: 双向箭头 + 文字 (居中, 标在线上方)."""
    ax.plot([x1, x2], [y, y], color=color, lw=0.9, zorder=5,
            solid_capstyle='butt')
    # 双向箭头
    ax.add_patch(FancyArrowPatch((x1, y), (x1+2.0, y),
                                 arrowstyle='-|>', mutation_scale=12,
                                 color=color, lw=0.9, zorder=6))
    ax.add_patch(FancyArrowPatch((x2, y), (x2-2.0, y),
                                 arrowstyle='-|>', mutation_scale=12,
                                 color=color, lw=0.9, zorder=6))
    # 文字 (线下方, 居中)
    ax.text((x1+x2)/2, y - 1.6, text,
            ha='center', va='top', fontsize=9, color=color, fontweight='bold')


def draw_v_dim(ax, x, y1, y2, text, color='#1d4ed8'):
    """垂直尺寸."""
    ax.plot([x, x], [y1, y2], color=color, lw=0.9, zorder=5)
    ax.add_patch(FancyArrowPatch((x, y1), (x, y1+2.0),
                                 arrowstyle='-|>', mutation_scale=12,
                                 color=color, lw=0.9, zorder=6))
    ax.add_patch(FancyArrowPatch((x, y2), (x, y2-2.0),
                                 arrowstyle='-|>', mutation_scale=12,
                                 color=color, lw=0.9, zorder=6))
    # 文字 (左侧, 垂直旋转 90)
    ax.text(x - 1.8, (y1+y2)/2, text,
            ha='center', va='center', fontsize=9,
            color=color, fontweight='bold', rotation=90)


def draw_plate(ax, cfg, x0, y0, label):
    """在某位置 (x0,y0) 为板左下角画板子."""
    W, H = cfg['W'], cfg['H']

    # 板轮廓
    ax.add_patch(Rectangle((x0, y0), W, H, facecolor='#f1f5f9',
                           edgecolor='#1e293b', linewidth=1.5, zorder=2))

    # 中心线
    ax.plot([x0+W/2]*2, [y0-2, y0+H+2],
            color='#94a3b8', lw=0.6, ls=(0,(2.5,1.5)), zorder=1)
    ax.plot([x0-2, x0+W+2], [y0+H/2]*2,
            color='#94a3b8', lw=0.6, ls=(0,(2.5,1.5)), zorder=1)

    # 内蓝孔
    for cx, cy in inner_xy(cfg):
        ax.add_patch(Circle((x0+cx, y0+cy), cfg['inner_d']/2,
                            facecolor='#3b82f6', edgecolor='#1d4ed8',
                            lw=0.7, zorder=4))
        cr = cfg['inner_d'] * 0.9
        ax.plot([x0+cx-cr, x0+cx+cr], [y0+cy, y0+cy],
                color='#1d4ed8', lw=0.4, ls=(0,(1,0.8)), zorder=3)
        ax.plot([x0+cx, x0+cx], [y0+cy-cr, y0+cy+cr],
                color='#1d4ed8', lw=0.4, ls=(0,(1,0.8)), zorder=3)

    # 外橙孔
    for cx, cy in outer_xy(cfg):
        ax.add_patch(Circle((x0+cx, y0+cy), cfg['outer_d']/2,
                            facecolor='#fb923c', edgecolor='#c2410c',
                            lw=0.7, zorder=4))
        cr = cfg['outer_d'] * 0.9
        ax.plot([x0+cx-cr, x0+cx+cr], [y0+cy, y0+cy],
                color='#c2410c', lw=0.4, ls=(0,(1,0.8)), zorder=3)
        ax.plot([x0+cx, x0+cx], [y0+cy-cr, y0+cy+cr],
                color='#c2410c', lw=0.4, ls=(0,(1,0.8)), zorder=3)

    # 板标题
    ax.text(x0 + W/2, y0 + H + 4,
            f"{label}    {W:.0f} × {H:.0f} mm",
            ha='center', va='bottom',
            fontsize=12, fontweight='bold', color=cfg['title_color'])

    # 标注: 外形 W (下)
    dim_y = y0 - 8
    draw_h_dim(ax, x0, x0+W, dim_y, f"{W:.2f}")

    # 标注: 外形 H (左)
    dim_x = x0 - 8
    draw_v_dim(ax, dim_x, y0, y0+H, f"{H:.2f}")

    # 内孔水平间距
    cx_center = x0 + W/2
    iy = y0 + H - 4
    draw_h_dim(ax, cx_center - cfg['inner_long']/2,
               cx_center + cfg['inner_long']/2, iy,
               f"{cfg['inner_long']:.2f}")

    # 内孔垂直间距
    ix = x0 + W - 4
    draw_v_dim(ax, ix, y0 + cfg['inner_short']/2,
               y0 + H - cfg['inner_short']/2,
               f"{cfg['inner_short']:.2f}")


def main():
    fig, ax = plt.subplots(figsize=(15, 11), dpi=140)
    fig.patch.set_facecolor('white')
    ax.set_facecolor('white')

    # 布局: 上板 y=0 起始, X 偏移
    tx0, ty0 = 25, 80
    draw_plate(ax, TOP, tx0, ty0, "Top Plate")

    # 下板 (放在上板下方, 留足够空)
    gap = 50
    by0 = ty0 - TOP['H'] - gap
    bx0 = tx0
    draw_plate(ax, BOT, bx0, by0, "Bottom Plate")

    # 标题
    ax.text(25, ty0 + TOP['H'] + 16,
            "Acrylic Plate CAD Layout",
            fontsize=18, fontweight='bold', color='#0f172a')
    ax.text(25, ty0 + TOP['H'] + 10,
            f"Unit: mm (1:1)   |   Material: Transparent Acrylic   |   "
            f"Thickness: {PLATE_THICKNESS} mm",
            fontsize=10, color='#475569')

    # Notes & 图例 (放在下板下方)
    notes_y_top = by0 - 22
    ax.text(25, notes_y_top, "Notes",
            fontsize=12, fontweight='bold', color='#0f172a')

    notes = [
        "* Top plate inner holes mount the gimbal base (云台底座).",
        "* Bottom plate inner holes mount the ESP32 controller (控制板).",
        "* 4 corner holes are co-axial through both plates, joined by 4 hex "
        "standoffs with M3 long/short screws.",
    ]
    for i, line in enumerate(notes):
        ax.text(25, notes_y_top - 7 - i*5, line, fontsize=9, color='#475569')

    # 图例 (放在右上角, 远离板标题)
    leg_x = 145
    leg_y = ty0 - 10
    ax.add_patch(Circle((leg_x, leg_y), 1.3,
                        facecolor='#3b82f6', edgecolor='#1d4ed8', lw=0.7))
    ax.text(leg_x+3, leg_y,
            f"Ø{TOP['inner_d']} mm inner hole (M2 screw)",
            fontsize=9, va='center', color='#334155')

    leg_y -= 7
    ax.add_patch(Circle((leg_x, leg_y), 1.3,
                        facecolor='#fb923c', edgecolor='#c2410c', lw=0.7))
    ax.text(leg_x+3, leg_y,
            f"Ø{TOP['outer_d']} mm outer hole (M3 hex standoff)",
            fontsize=9, va='center', color='#334155')

    leg_y -= 7
    ax.plot([leg_x-3, leg_x+3], [leg_y, leg_y], color='#1d4ed8', lw=1.2)
    ax.add_patch(FancyArrowPatch((leg_x-3, leg_y), (leg_x-1.5, leg_y),
                                 arrowstyle='-|>', mutation_scale=10,
                                 color='#1d4ed8', lw=1.2))
    ax.add_patch(FancyArrowPatch((leg_x+3, leg_y), (leg_x+1.5, leg_y),
                                 arrowstyle='-|>', mutation_scale=10,
                                 color='#1d4ed8', lw=1.2))
    ax.text(leg_x+6, leg_y, "Dimension in mm",
            fontsize=9, va='center', color='#334155')

    # 关闭坐标轴
    ax.set_xlim(-15, 220)
    ax.set_ylim(by0 - 38, ty0 + TOP['H'] + 22)
    ax.set_aspect('equal')
    ax.axis('off')

    out = r'C:\Users\20455\WorkBuddy\2026-08-26-13-09-10\acrylic_plates_cad_preview.png'
    fig.savefig(out, bbox_inches='tight', facecolor='white', dpi=160)
    print(f"[OK] PNG -> {out}")


if __name__ == "__main__":
    main()
