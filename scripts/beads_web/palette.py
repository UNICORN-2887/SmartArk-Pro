"""36 色水彩笔色板 — 唯一数据源，逐字移植设备端 beads_storage.cc 的 g_beads_palette。

设备端色卡 UI 布局（beads_ui.cc:401-425）：6×6 网格，索引 i → 列 i%6、行 i//6。
本模块的 palette_coord(i) 返回 1-based (行, 列) = (i//6+1, i%6+1)，与设备色卡一致，
用于对照图上每格的色卡坐标标注。
"""

# 与 main/apps/beads/beads_storage.cc:13-26 逐项一致（RGB565，行序 红/橙黄/绿/蓝/紫粉/灰黑）
PALETTE_RGB565 = [
    # 行1 红系
    0x8800, 0x7808, 0xF800, 0xF8C0, 0xF812, 0xFD16,
    # 行2 橙黄系
    0x5AC2, 0x4282, 0x6A63, 0xFC60, 0xFD00, 0xFFE0,
    # 行3 绿系
    0x0320, 0x4AA4, 0x0400, 0x3666, 0x97D2, 0xAFA5,
    # 行4 蓝系
    0x188E, 0x0011, 0x001F, 0x05FF, 0xADDC, 0x07FF,
    # 行5 紫粉系
    0x4810, 0x8010, 0xBAB6, 0xDB92, 0xE73F, 0xFFFF,
    # 行6 灰黑系
    0x0000, 0x4208, 0x8410, 0xC618, 0xB31B, 0xFFDF,
]


def rgb565_to_rgb888(c: int):
    """RGB565 → RGB888，与 beads_ui.cc:412-414 位移公式一致。"""
    return ((c >> 11) & 0x1F) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3


PALETTE_RGB = [rgb565_to_rgb888(c) for c in PALETTE_RGB565]
BEADS_COLORS = 36


def palette_coord(i: int):
    """色号索引 i(0..35) → 设备 6×6 色卡 1-based (行, 列)。"""
    return i // 6 + 1, i % 6 + 1


def color_id_to_rgb(color_id: int):
    """色号 1..36 → RGB888 tuple。"""
    return PALETTE_RGB[color_id - 1]
