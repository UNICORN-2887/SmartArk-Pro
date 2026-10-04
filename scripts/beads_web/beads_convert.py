"""拼豆转换核心算法 — 纯 numpy/struct，无 Flask/PIL 依赖，可独立测试。

管线：平均池化（浮点边界加权）→ 36 色量化 → 派生切块 → .bead 二进制。
关键约束：只量化一次 N×N，所有派生物（B3/B2/B1）都是它的切片，
保证 B3 的 72 块与导出的 B2 文件、B2 的 24 块与导出的 B1 文件逐字节一致。
"""

import struct
import numpy as np

from palette import PALETTE_RGB, BEADS_COLORS

# 尺寸常量，与设备端 beads.h 一致
BEADS_L1 = 24
BEADS_L2 = 72
BEADS_L3 = 216


# ── 池化 ──

def _pool1d_axis0(x2d, n):
    """沿 axis=0 把 (H, W) 池化成 (n, W)。

    浮点边界加权面积平均：输出 i 覆盖源区间 [i*H/n, (i+1)*H/n)，
    边界行按小数权重计入（用连续前缀和 F(t) = 前 floor(t) 行之和 + x[floor(t)] * frac(t)），
    非整数比例不丢边、不偏置。
    """
    H, W = x2d.shape
    lo = np.arange(n, dtype=np.float64) * H / n
    hi = np.arange(1, n + 1, dtype=np.float64) * H / n

    ilo = np.floor(lo).astype(np.int64)
    flo = lo - ilo
    ihi = np.floor(hi).astype(np.int64)
    fhi = hi - ihi

    # 前后各 pad 一行 0：xp[k] = x[k-1]，使 C[k] = 前 k 行之和，边界越界安全
    # float64 累积，避免长行前缀和的 float32 舍入误差
    xp = np.vstack([np.zeros((1, W), dtype=np.float64), x2d.astype(np.float64),
                    np.zeros((1, W), dtype=np.float64)])
    C = np.cumsum(xp, axis=0)  # (H+2, W)

    F_lo = C[ilo] + xp[ilo + 1] * flo[:, None]
    F_hi = C[ihi] + xp[ihi + 1] * fhi[:, None]
    return (F_hi - F_lo) / (hi - lo)[:, None]


def average_pool(img, n):
    """img: (H, W, 3) uint8/float → (n, n, 3) float32 面积平均池化。

    2D 权重是行列权重的乘积，故可分离：先沿行池化再沿列池化。
    """
    H, W, C = img.shape
    a = _pool1d_axis0(img.reshape(H, W * C), n).reshape(n, W, C)   # 行池化
    a = a.transpose(1, 0, 2)                                        # (W, n, C)
    a = _pool1d_axis0(a.reshape(W, n * C), n).reshape(n, n, C)      # 列池化
    return a.transpose(1, 0, 2).astype(np.float32)


# ── 36 色量化 ──

def quantize(pooled):
    """(n, n, 3) float → (n, n) uint8 色号 1..36，RGB 欧氏距离最近邻。"""
    n = pooled.shape[0]
    pal = np.asarray(PALETTE_RGB, dtype=np.float32)          # (36, 3)
    p = pooled.astype(np.float32).reshape(-1, 3)             # (n*n, 3)
    diff = p[:, None, :] - pal[None, :, :]                   # (n*n, 36, 3)
    dist = (diff * diff).sum(-1)                             # (n*n, 36)
    return (dist.argmin(-1).astype(np.uint8) + 1).reshape(n, n)


# ── .bead 二进制（逐字节对应 beads_storage.cc:49-69）──

def make_bead(level, art):
    """art: (h, w) uint8 色号 1..36 → .bead 字节。

    格式：magic 'BEAD'(4B) + level(1B) + w(2B 小端) + h(2B 小端) + w*h 像素。
    """
    h, w = art.shape
    return struct.pack('<4sBHH', b'BEAD', level, w, h) + art.astype(np.uint8).tobytes()


# ── 派生切块与导出包 ──

def build_export(art_N, n, name):
    """art_N: (n, n) 色号矩阵 → 导出包。

    返回 dict:
      beads: [(文件名, bytes)] 全套 .bead（B3+B2+B1 或子集）
      tiles: [dict(basename, pos216, pos72, art24)]  B1 24×24 块（对照图用）
      b2s:   [dict(basename, pos216, art72)]         B2 72×72 块（辅助图用，n=216 时 9 个）
    pos216/pos72: (行, 列) 1-based，该级别不存在时为 None。
    """
    beads = []
    tiles = []
    b2s = []

    def b1_name(r, c):
        return f'B1_{name}_r{r}c{c}'

    def b2_name(r, c):
        return f'B2_{name}_r{r}c{c}'

    if n == BEADS_L1:  # 24：仅 1 张 B1
        beads.append((f'B1_{name}.bead', make_bead(1, art_N)))
        tiles.append(dict(basename=f'B1_{name}', pos216=None, pos72=None, art=art_N))

    elif n == BEADS_L2:  # 72：1 张 B2 + 9 张 B1
        beads.append((f'B2_{name}.bead', make_bead(2, art_N)))
        for r in range(1, 4):
            for c in range(1, 4):
                art24 = art_N[(r - 1) * 24:r * 24, (c - 1) * 24:c * 24]
                beads.append((f'{b1_name(r, c)}.bead', make_bead(1, art24)))
                tiles.append(dict(basename=b1_name(r, c), pos216=None,
                                  pos72=(r, c), art=art24))

    else:  # 216：1 张 B3 + 9 张 B2 + 81 张 B1
        beads.append((f'B3_{name}.bead', make_bead(3, art_N)))
        for rb in range(1, 4):
            for cb in range(1, 4):
                art72 = art_N[(rb - 1) * 72:rb * 72, (cb - 1) * 72:cb * 72]
                beads.append((f'{b2_name(rb, cb)}.bead', make_bead(2, art72)))
                b2s.append(dict(basename=b2_name(rb, cb), pos216=(rb, cb), art=art72))
                for r in range(1, 4):
                    for c in range(1, 4):
                        art24 = art72[(r - 1) * 24:r * 24, (c - 1) * 24:c * 24]
                        beads.append((f'{b1_name((rb - 1) * 3 + r, (cb - 1) * 3 + c)}.bead',
                                      make_bead(1, art24)))
                        tiles.append(dict(
                            basename=b1_name((rb - 1) * 3 + r, (cb - 1) * 3 + c),
                            pos216=((rb - 1) * 3 + r, (cb - 1) * 3 + c),
                            pos72=(rb, cb), art=art24))

    return dict(beads=beads, tiles=tiles, b2s=b2s)
