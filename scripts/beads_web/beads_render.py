"""对照图 PNG 渲染 — PIL 绘制，图内文字全 ASCII（内置字体无 CJK），中文只出现在 HTML 层。

对照图样式（用户指定）：
- 每张 24×24 块放大展示（cell=20px → 480×480）
- 顶部信息条标注块在 216(9×9)/72(3×3) 矩阵中的位置
- 格子里填 36 色对应色，每格中央写该色在设备 6×6 色卡上的坐标（行-列）
"""

from PIL import Image, ImageDraw, ImageFont

from palette import PALETTE_RGB, palette_coord

INFO_H = 28  # 块图顶部信息条高度


def _font(size):
    return ImageFont.load_default(size=size)


_TEXT_CACHE = {}  # (txt, fill) → RGBA 小图：576 格 × 81 张的文本光栅化只做 72 次


def _text_img(txt, font, fill):
    key = (txt, fill)
    if key not in _TEXT_CACHE:
        bbox = font.getbbox(txt)
        w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
        ti = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(ti).text((-bbox[0], -bbox[1]), txt, font=font, fill=fill)
        _TEXT_CACHE[key] = ti
    return _TEXT_CACHE[key]


def _text_center(img, cx, cy, txt, font, fill):
    ti = _text_img(txt, font, fill)
    img.paste(ti, (int(cx - ti.width / 2), int(cy - ti.height / 2)), ti)


def _luma(rgb):
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def _info_line(pos216, pos72):
    """位置标注文字：216: 9x9 row3 col2 | 72: 3x3 row1 col1（不存在写 N/A）。"""
    t216 = f'row{pos216[0]} col{pos216[1]}' if pos216 else 'N/A'
    t72 = f'row{pos72[0]} col{pos72[1]}' if pos72 else 'N/A'
    return f'216: 9x9 {t216}   |   72: 3x3 {t72}'


def render_tile_png(art24, pos216, pos72, cell=20):
    """24×24 块对照图：(24*cell) × (24*cell + 信息条)，每格色块 + 网格线 + 色卡坐标。"""
    W = 24 * cell
    img = Image.new('RGB', (W, W + INFO_H), 'white')
    d = ImageDraw.Draw(img)
    d.text((4, 6), _info_line(pos216, pos72), font=_font(14), fill='black')

    font_txt = _font(10)
    for r in range(24):
        for c in range(24):
            color = PALETTE_RGB[art24[r, c] - 1]
            x0, y0 = c * cell, INFO_H + r * cell
            d.rectangle([x0, y0, x0 + cell - 1, y0 + cell - 1], fill=color)
            # 网格线（浅灰，先于文本）
            d.line([(x0, y0), (x0 + cell - 1, y0)], fill=(190, 190, 190))
            d.line([(x0, y0), (x0, y0 + cell - 1)], fill=(190, 190, 190))
            # 色卡坐标（如 3-2）
            row, col = palette_coord(art24[r, c] - 1)
            fill = 'black' if _luma(color) > 128 else 'white'
            _text_center(img, x0 + cell / 2, y0 + cell / 2, f'{row}-{col}', font_txt, fill)
    # 外边框
    d.rectangle([0, INFO_H, W - 1, W + INFO_H - 1], outline=(190, 190, 190))
    return img


def render_overview_png(art_N, n, cell):
    """总览图：n×n 放大，浅线每 24 格、黑线每 72 格（n≥24 时）。"""
    W = n * cell
    img = Image.new('RGB', (W, W), 'white')
    d = ImageDraw.Draw(img)
    for r in range(n):
        for c in range(n):
            d.rectangle([c * cell, r * cell, (c + 1) * cell - 1, (r + 1) * cell - 1],
                        fill=PALETTE_RGB[art_N[r, c] - 1])
    if n >= 24:
        step = 24 * cell
        for i in range(0, W + 1, step):
            d.line([(i, 0), (i, W)], fill=(200, 200, 200))
            d.line([(0, i), (W, i)], fill=(200, 200, 200))
    if n >= 72:
        step = 72 * cell
        for i in range(0, W + 1, step):
            d.line([(i, 0), (i, W)], fill=(0, 0, 0), width=2)
            d.line([(0, i), (W, i)], fill=(0, 0, 0), width=2)
    return img


def render_b2_png(art72, pos216, cell=6):
    """72×72 块辅助图：色块 + 每 24 格浅线 + 外框黑线，信息条标 216 位置。"""
    W = 72 * cell
    img = Image.new('RGB', (W, W + INFO_H), 'white')
    d = ImageDraw.Draw(img)
    d.text((4, 6), _info_line(pos216, None), font=_font(14), fill='black')
    for r in range(72):
        for c in range(72):
            d.rectangle([c * cell, INFO_H + r * cell, (c + 1) * cell - 1,
                         INFO_H + (r + 1) * cell - 1], fill=PALETTE_RGB[art72[r, c] - 1])
    for i in range(0, 73, 24):  # 3×3 分隔浅线
        x = i * cell
        y = INFO_H + i * cell
        d.line([(x, INFO_H), (x, W + INFO_H)], fill=(150, 150, 150))
        d.line([(0, y), (W, y)], fill=(150, 150, 150))
    d.rectangle([0, INFO_H, W - 1, W + INFO_H - 1], outline=(0, 0, 0), width=2)
    return img


def render_palette_png(cell=64):
    """36 色卡说明图：6×6 布局与设备色卡一致，每格标色号 + 色卡坐标。"""
    TITLE_H = 34
    W = 6 * cell
    img = Image.new('RGB', (W, W + TITLE_H), 'white')
    d = ImageDraw.Draw(img)
    d.text((4, 8), 'Beads 36-color palette  (color# / row-col on device 6x6 card)',
           font=_font(16), fill='black')
    f_num = _font(22)
    f_coord = _font(14)
    for i in range(36):
        r, c = i // 6, i % 6
        color = PALETTE_RGB[i]
        x0, y0 = c * cell, TITLE_H + r * cell
        d.rectangle([x0, y0, x0 + cell - 1, y0 + cell - 1], fill=color)
        fill = 'black' if _luma(color) > 128 else 'white'
        row, col = palette_coord(i)
        _text_center(img, x0 + cell / 2, y0 + cell / 2 - 10, str(i + 1), f_num, fill)
        _text_center(img, x0 + cell / 2, y0 + cell / 2 + 12, f'{row}-{col}', f_coord, fill)
    return img
