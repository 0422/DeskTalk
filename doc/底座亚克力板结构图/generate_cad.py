#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
根据截图生成两块亚克力板 (上板/下板) 的 CAD 图。
输出:
  - acrylic_plates.svg  (矢量预览，方便查看)
  - acrylic_plates.dxf  (AutoCAD / 激光切割机可直接打开)
"""

from pathlib import Path
import ezdxf
from ezdxf.enums import TextEntityAlignment

# =============================================================================
# 参数 (mm)
# =============================================================================
PLATE_THICKNESS = 3.0   # 板厚 (用户未明确, 按常规亚克力厚度假设)

# 上板
TOP = dict(
    name="Top Plate (上板)",
    W=80.0, H=55.0,
    inner_long=57.15,    # 内孔长边孔心间距
    inner_short=27.94,   # 内孔短边孔心间距
    inner_d=2.2,         # M2 螺丝孔
    outer_d=3.2,         # 外角孔
    outer_margin=3.0,    # 外角孔距边缘 (估计)
)
# 下板
BOT = dict(
    name="Bottom Plate (下板)",
    W=80.0, H=55.0,
    inner_long=44.32,
    inner_short=22.00,
    inner_d=2.2,
    outer_d=3.2,
    outer_margin=3.0,
)


def inner_holes(cfg):
    """内孔 4 个中心 (相对板左下角, mm)。"""
    dx = (cfg['W'] - cfg['inner_long']) / 2
    dy = (cfg['H'] - cfg['inner_short']) / 2
    return [(dx, dy),
            (cfg['W'] - dx, dy),
            (dx, cfg['H'] - dy),
            (cfg['W'] - dx, cfg['H'] - dy)]


def outer_holes(cfg):
    """外角孔 4 个中心。"""
    m = cfg['outer_margin']
    return [(m, m),
            (cfg['W'] - m, m),
            (m, cfg['H'] - m),
            (cfg['W'] - m, cfg['H'] - m)]


for cfg in (TOP, BOT):
    cfg['inner'] = inner_holes(cfg)
    cfg['outer'] = outer_holes(cfg)


# =============================================================================
# SVG 生成 (单位: mm, 1:1)
# =============================================================================
def make_plate_svg_body(cfg, plate_label):
    """单块板的 SVG body (局部坐标, 相对板左下角)。"""
    W, H = cfg['W'], cfg['H']
    s = []
    s.append('<g class="plate-group">')

    # 板轮廓
    s.append(f'<rect x="0" y="0" width="{W}" height="{H}" '
             f'class="plate-fill" rx="0.5" />')

    # 中心线 (点划线)
    s.append(f'<line x1="{W/2}" y1="-2" x2="{W/2}" y2="{H+2}" class="center-line" />')
    s.append(f'<line x1="-2" y1="{H/2}" x2="{W+2}" y2="{H/2}" class="center-line" />')

    # 内蓝孔 (M2 螺丝孔)
    for cx, cy in cfg['inner']:
        s.append(f'<circle cx="{cx}" cy="{cy}" r="{cfg["inner_d"]/2}" '
                 f'class="hole-blue" />')

    # 外橙孔 (六角铜柱孔)
    for cx, cy in cfg['outer']:
        s.append(f'<circle cx="{cx}" cy="{cy}" r="{cfg["outer_d"]/2}" '
                 f'class="hole-orange" />')

    # ---- 尺寸标注 ----
    # 外形 W (下)
    dy = H + 6
    s += dim_h(0, W, dy, label=f"{W:.2f}", text_offset=(0, 2.5), anchor='top')

    # 外形 H (左)
    dx = -6
    s += dim_v(dx, 0, H, label=f"{H:.2f}", text_offset=(-1, 0), anchor='end')

    # 内孔水平
    xL, _ = cfg['inner'][0]
    xR, _ = cfg['inner'][1]
    dy2 = H - 3
    s += dim_h(xL, xR, dy2, label=f"{cfg['inner_long']:.2f}",
               text_offset=(0, -1.0), anchor='bottom')
    # 引线
    s.append(f'<line class="dim-line" x1="{xL}" y1="{dy2}" '
             f'x2="{xL}" y2="{dy2+2}" />')
    s.append(f'<line class="dim-line" x1="{xR}" y1="{dy2}" '
             f'x2="{xR}" y2="{dy2+2}" />')

    # 内孔垂直
    _, yB = cfg['inner'][0]
    _, yT = cfg['inner'][2]
    dx2 = W - 3
    s += dim_v(dx2, yB, yT, label=f"{cfg['inner_short']:.2f}",
               text_offset=(0.8, 0), anchor='start')
    # 引线
    s.append(f'<line class="dim-line" x1="{dx2}" y1="{yB}" '
             f'x2="{dx2-2}" y2="{yB}" />')
    s.append(f'<line class="dim-line" x1="{dx2}" y1="{yT}" '
             f'x2="{dx2-2}" y2="{yT}" />')

    # 板名
    s.append(f'<text x="{W/2}" y="-9" class="subtitle" '
             f'text-anchor="middle">{plate_label} - {cfg["W"]} × {cfg["H"]} mm</text>')

    s.append('</g>')
    return '\n'.join(s)


def dim_h(x1, x2, y, label, text_offset=(0, 0), anchor='middle'):
    """水平尺寸线 + 双向箭头 + 文字。"""
    s = []
    s.append(f'<line class="dim-line" x1="{x1}" y1="{y}" '
             f'x2="{x2}" y2="{y}" />')
    # 箭头 (V 形, 三角)
    s.append(f'<polygon class="dim-arrow" '
             f'points="{x1},{y} {x1+2},{y-0.9} {x1+2},{y+0.9}" />')
    s.append(f'<polygon class="dim-arrow" '
             f'points="{x2},{y} {x2-2},{y-0.9} {x2-2},{y+0.9}" />')
    # 延长线
    s.append(f'<line class="dim-ext" x1="{x1}" y1="{y-1.5}" '
             f'x2="{x1}" y2="{y+2}" />')
    s.append(f'<line class="dim-ext" x1="{x2}" y1="{y-1.5}" '
             f'x2="{x2}" y2="{y+2}" />')
    # 文字
    cx = (x1 + x2) / 2 + text_offset[0]
    cy = y + text_offset[1]
    s.append(f'<text x="{cx}" y="{cy}" class="dim-text" '
             f'text-anchor="{anchor}">{label}</text>')
    return s


def dim_v(x, y1, y2, label, text_offset=(0, 0), anchor='middle'):
    """垂直尺寸线。"""
    s = []
    s.append(f'<line class="dim-line" x1="{x}" y1="{y1}" '
             f'x2="{x}" y2="{y2}" />')
    s.append(f'<polygon class="dim-arrow" '
             f'points="{x},{y1} {x-0.9},{y1+2} {x+0.9},{y1+2}" />')
    s.append(f'<polygon class="dim-arrow" '
             f'points="{x},{y2} {x-0.9},{y2-2} {x+0.9},{y2-2}" />')
    s.append(f'<line class="dim-ext" x1="{x-2}" y1="{y1}" '
             f'x2="{x+1.5}" y2="{y1}" />')
    s.append(f'<line class="dim-ext" x1="{x-2}" y1="{y2}" '
             f'x2="{x+1.5}" y2="{y2}" />')
    cx = x + text_offset[0]
    cy = (y1 + y2) / 2 + text_offset[1]
    s.append(f'<text x="{cx}" y="{cy}" class="dim-text" '
             f'text-anchor="{anchor}" '
             f'transform="rotate(-90 {cx} {cy})">{label}</text>')
    return s


def build_svg():
    margin_x = 18           # 板左右标注余量
    gap = 28                # 上下板间距
    title_h = 18
    foot_h = 26

    total_w = margin_x * 2 + max(TOP['W'], BOT['W'])
    total_h = title_h + TOP['H'] + gap + BOT['H'] + foot_h

    s = []
    s.append('<?xml version="1.0" encoding="UTF-8"?>')
    s.append(f'<svg xmlns="http://www.w3.org/2000/svg" '
             f'viewBox="0 0 {total_w} {total_h}" '
             f'width="{total_w*4}px" height="{total_h*4}px" '
             f'font-family="Arial, sans-serif">')
    s.append('''<style>
        .bg { fill: #ffffff; }
        .title { font: bold 6px sans-serif; fill: #0f172a; }
        .subtitle { font: bold 4px sans-serif; fill: #1e293b; }
        .body { font: 3.4px sans-serif; fill: #334155; }
        .dim-text { font: 3.2px sans-serif; fill: #1d4ed8; font-weight: 600; }
        .plate-fill { fill: #f1f5f9; stroke: #1e40af; stroke-width: 0.25; }
        .hole-blue { fill: #3b82f6; stroke: #1d4ed8; stroke-width: 0.15; }
        .hole-orange { fill: #fb923c; stroke: #c2410c; stroke-width: 0.15; }
        .dim-line { stroke: #1d4ed8; stroke-width: 0.2; fill: none; }
        .dim-arrow { fill: #1d4ed8; stroke: none; }
        .dim-ext { stroke: #1d4ed8; stroke-width: 0.15; fill: none; }
        .center-line { stroke: #94a3b8; stroke-width: 0.15;
                        stroke-dasharray: 1.5 0.8; fill: none; }
        .legend-color { stroke-width: 0.15; }
    </style>''')
    s.append(f'<rect class="bg" x="0" y="0" width="{total_w}" height="{total_h}" />')

    # 标题
    s.append(f'<text x="{margin_x}" y="8" class="title">'
             f'Acrylic Plate CAD Layout · 亚克力板结构图</text>')
    s.append(f'<text x="{margin_x}" y="14" class="body">'
             f'Unit: mm (1:1) · Material: 透明亚克力 · Thickness: {PLATE_THICKNESS} mm'
             f'</text>')

    # 上板
    ty0 = title_h
    s.append(f'<g transform="translate({margin_x},{ty0})">')
    s.append(make_plate_svg_body(TOP, "Top Plate / 上板"))
    s.append('</g>')

    # 下板
    by0 = title_h + TOP['H'] + gap
    s.append(f'<g transform="translate({margin_x},{by0})">')
    s.append(make_plate_svg_body(BOT, "Bottom Plate / 下板"))
    s.append('</g>')

    # 图例 + 注释
    leg_y = title_h + TOP['H'] + gap + BOT['H'] + 8
    s.append(f'<text x="{margin_x}" y="{leg_y}" class="subtitle">'
             f'图例 Legend</text>')

    # 内蓝孔示例
    s.append(f'<circle cx="{margin_x+3}" cy="{leg_y+5}" r="1.1" '
             f'class="hole-blue legend-color" />')
    s.append(f'<text x="{margin_x+7}" y="{leg_y+6}" class="body">'
             f'内孔 Ø{TOP["inner_d"]} mm (M2 螺丝孔 · M2 Screw)</text>')

    # 外橙孔示例
    s.append(f'<circle cx="{margin_x+50}" cy="{leg_y+5}" r="1.6" '
             f'class="hole-orange legend-color" />')
    s.append(f'<text x="{margin_x+54}" y="{leg_y+6}" class="body">'
             f'外角孔 Ø{TOP["outer_d"]} mm (六角铜柱 · Hex standoff)</text>')

    # 尺寸线示例
    s.append(f'<line class="dim-line" x1="{margin_x+108}" y1="{leg_y+5}" '
             f'x2="{margin_x+118}" y2="{leg_y+5}" />')
    s.append(f'<polygon class="dim-arrow" '
             f'points="{margin_x+108},{leg_y+5} '
             f'{margin_x+110},{leg_y+4.1} {margin_x+110},{leg_y+5.9}" />')
    s.append(f'<polygon class="dim-arrow" '
             f'points="{margin_x+118},{leg_y+5} '
             f'{margin_x+116},{leg_y+4.1} {margin_x+116},{leg_y+5.9}" />')
    s.append(f'<text x="{margin_x+120}" y="{leg_y+6}" class="body">'
             f'尺寸标注 Dimension (mm)</text>')

    # Notes
    s.append(f'<text x="{margin_x}" y="{leg_y+13}" class="body">'
             f'· Top plate 内孔: 安装云台底座 (Gimbal Base Mount)</text>')
    s.append(f'<text x="{margin_x}" y="{leg_y+18}" class="body">'
             f'· Bottom plate 内孔: 安装 ESP32 控制板 (Controller Board)</text>')
    s.append(f'<text x="{margin_x}" y="{leg_y+23}" class="body">'
             f'· 外角孔上下对位, 通过 4 颗六角铜柱 + M3 长/短螺丝锁紧上下板间距.'
             f'</text>')

    s.append('</svg>')
    return '\n'.join(s)


# =============================================================================
# DXF 生成
# =============================================================================
def build_dxf():
    doc = ezdxf.new(dxfversion='R2010', setup=True)
    # 加载标准线型
    if 'DASHED' not in doc.linetypes:
        doc.linetypes.new('DASHED', dxfattribs={'pattern': '1.0 0.5'})

    doc.layers.add('OUTLINE',       color=7)    # 白
    doc.layers.add('INNER_HOLES',   color=5)    # 蓝
    doc.layers.add('OUTER_HOLES',   color=30)   # 橙
    doc.layers.add('DIMENSIONS',    color=1)    # 红
    doc.layers.add('CENTER',        color=8)    # 灰
    doc.layers.add('TEXT',          color=7)

    msp = doc.modelspace()

    gap_x = 20      # 两板水平间距
    margin_l = 15   # 左侧标注余量

    for idx, cfg in enumerate((TOP, BOT)):
        plate_tag = 'TOP' if idx == 0 else 'BOTTOM'
        ox = margin_l + idx * (cfg['W'] + gap_x)

        # 板标题
        msp.add_text(
            f"{plate_tag} - {cfg['name']}",
            dxfattribs={'layer': 'TEXT', 'height': 3.0},
        ).set_placement((ox, -14), align=TextEntityAlignment.BOTTOM_LEFT)

        # 板轮廓 (闭合矩形)
        rect_corners = [
            (ox, 0), (ox + cfg['W'], 0),
            (ox + cfg['W'], cfg['H']), (ox, cfg['H'])
        ]
        for j in range(4):
            msp.add_line(rect_corners[j], rect_corners[(j+1) % 4],
                         dxfattribs={'layer': 'OUTLINE'})

        # 中心线
        msp.add_line((ox + cfg['W']/2, -3), (ox + cfg['W']/2, cfg['H']+3),
                     dxfattribs={'layer': 'CENTER',
                                 'linetype': 'DASHED'})
        msp.add_line((ox-3, cfg['H']/2), (ox+cfg['W']+3, cfg['H']/2),
                     dxfattribs={'layer': 'CENTER',
                                 'linetype': 'DASHED'})

        # 内蓝孔
        for cx, cy in cfg['inner']:
            msp.add_circle((ox + cx, cy),
                           radius=cfg['inner_d']/2,
                           dxfattribs={'layer': 'INNER_HOLES'})
            # 中心十字
            cr = cfg['inner_d']
            msp.add_line((ox+cx-cr, cy), (ox+cx+cr, cy),
                         dxfattribs={'layer': 'CENTER',
                                     'linetype': 'DASHED'})
            msp.add_line((ox+cx, cy-cr), (ox+cx, cy+cr),
                         dxfattribs={'layer': 'CENTER',
                                     'linetype': 'DASHED'})

        # 外橙孔
        for cx, cy in cfg['outer']:
            msp.add_circle((ox + cx, cy),
                           radius=cfg['outer_d']/2,
                           dxfattribs={'layer': 'OUTER_HOLES'})
            cr = cfg['outer_d']
            msp.add_line((ox+cx-cr, cy), (ox+cx+cr, cy),
                         dxfattribs={'layer': 'CENTER',
                                     'linetype': 'DASHED'})
            msp.add_line((ox+cx, cy-cr), (ox+cx, cy+cr),
                         dxfattribs={'layer': 'CENTER',
                                     'linetype': 'DASHED'})

        # 标注 - W
        dim_y = -8
        msp.add_line((ox, dim_y), (ox + cfg['W'], dim_y),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_text(f"{cfg['W']:.2f}",
                     dxfattribs={'layer': 'DIMENSIONS',
                                 'height': 2.0}).set_placement(
            (ox + cfg['W']/2, dim_y - 1.5),
            align=TextEntityAlignment.TOP_CENTER)

        # 标注 - H
        dim_x = -8
        msp.add_line((dim_x, 0), (dim_x, cfg['H']),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_text(f"{cfg['H']:.2f}",
                     dxfattribs={'layer': 'DIMENSIONS',
                                 'height': 2.0}).set_placement(
            (dim_x - 2, cfg['H']/2),
            align=TextEntityAlignment.MIDDLE_RIGHT)

        # 内孔水平间距
        xL, _ = cfg['inner'][0]
        xR, _ = cfg['inner'][1]
        dy2 = cfg['H'] - 4
        msp.add_line((ox + xL, dy2), (ox + xR, dy2),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_line((ox + xL, dy2), (ox + xL, dy2+2.0),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_line((ox + xR, dy2), (ox + xR, dy2+2.0),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_text(f"{cfg['inner_long']:.2f}",
                     dxfattribs={'layer': 'DIMENSIONS',
                                 'height': 1.6}).set_placement(
            (ox + (xL+xR)/2, dy2 - 1.0),
            align=TextEntityAlignment.BOTTOM_CENTER)

        # 内孔垂直间距
        _, yB = cfg['inner'][0]
        _, yT = cfg['inner'][2]
        dx2 = cfg['W'] - 4
        msp.add_line((ox + dx2, yB), (ox + dx2, yT),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_line((ox + dx2, yB), (ox + dx2 - 2.0, yB),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_line((ox + dx2, yT), (ox + dx2 - 2.0, yT),
                     dxfattribs={'layer': 'DIMENSIONS'})
        msp.add_text(f"{cfg['inner_short']:.2f}",
                     dxfattribs={'layer': 'DIMENSIONS',
                                 'height': 1.6}).set_placement(
            (ox + dx2 + 1, (yB+yT)/2),
            align=TextEntityAlignment.MIDDLE_LEFT)

    # 整个图纸标题
    msp.add_text(
        "Acrylic Plate Set · 亚克力板结构图 (TOP & BOTTOM) · Unit: mm",
        dxfattribs={'layer': 'TEXT', 'height': 4.0},
    ).set_placement((margin_l, -22), align=TextEntityAlignment.BOTTOM_LEFT)

    msp.add_text(
        f"Plate thickness: {PLATE_THICKNESS} mm    "
        f"Outer holes: {TOP['outer_d']} mm (M3 hex standoff)    "
        f"Inner holes: {TOP['inner_d']} mm (M2 screw)",
        dxfattribs={'layer': 'TEXT', 'height': 2.5},
    ).set_placement((margin_l, -26), align=TextEntityAlignment.BOTTOM_LEFT)

    return doc


def main():
    out = Path("C:/Users/20455/WorkBuddy/2026-08-26-13-09-10")
    out.mkdir(exist_ok=True, parents=True)

    svg_path = out / "acrylic_plates.svg"
    svg_path.write_text(build_svg(), encoding="utf-8")
    print(f"[OK] SVG -> {svg_path}")

    dxf_path = out / "acrylic_plates.dxf"
    doc = build_dxf()
    doc.saveas(str(dxf_path))
    print(f"[OK] DXF -> {dxf_path}")


if __name__ == "__main__":
    main()
