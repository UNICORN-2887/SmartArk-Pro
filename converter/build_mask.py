#!/usr/bin/env python3
"""
从 PNG 序列生成 RLE 遮罩文件 (.mask)
每个像素 1bit: alpha>128 → 1(显示), 否则 → 0(透明)
"""
import struct, sys, os
import numpy as np
from PIL import Image

W, H = 480, 800
MARGIN_BOTTOM = 60  # 和 build_mjpeg.py 保持一致


def rle_encode(mask_flat):
    """将 1-bit mask 转为 RLE 流: [2B count][1B value]... """
    out = bytearray()
    i = 0
    n = len(mask_flat)
    while i < n:
        val = mask_flat[i]
        cnt = 1
        while i + cnt < n and mask_flat[i + cnt] == val and cnt < 65535:
            cnt += 1
        out += struct.pack('<HB', cnt, val)
        i += cnt
    return bytes(out)


def build_mask(png_dir, out_path):
    files = sorted([f for f in os.listdir(png_dir) if f.lower().endswith('.png')])
    if not files:
        print(f"  No PNGs in {png_dir}")
        return

    # 先用第一帧算角色位置
    first = Image.open(os.path.join(png_dir, files[0]))
    if first.mode != 'RGBA':
        first = first.convert('RGBA')
    fw, fh = first.size
    x_off = (W - fw) // 2
    y_off = H - fh - MARGIN_BOTTOM

    masks = []
    for fn in files:
        img = Image.open(os.path.join(png_dir, fn))
        if img.mode != 'RGBA':
            img = img.convert('RGBA')

        arr = np.array(img)
        alpha = arr[:, :, 3]
        char_mask = (alpha > 128).astype(np.uint8)  # 角色区域的二值遮罩

        # 放置到 480×800 画布的正确位置
        full_mask = np.zeros((H, W), dtype=np.uint8)

        # clip 到画布范围内
        src_y1 = max(0, -y_off)
        src_y2 = min(fh, H - y_off)
        dst_y1 = max(0, y_off)
        dst_y2 = min(H, y_off + fh)
        src_x1 = max(0, -x_off)
        src_x2 = min(fw, W - x_off)
        dst_x1 = max(0, x_off)
        dst_x2 = min(W, x_off + fw)

        h = src_y2 - src_y1
        w = src_x2 - src_x1
        if h > 0 and w > 0:
            full_mask[dst_y1:dst_y1+h, dst_x1:dst_x1+w] = char_mask[src_y1:src_y1+h, src_x1:src_x1+w]

        masks.append(rle_encode(full_mask.flatten()))

    fc = len(masks)
    header_size = 4 + fc * 4
    offsets = []
    off = header_size
    for m in masks:
        offsets.append(off)
        off += len(m)

    with open(out_path, 'wb') as f:
        f.write(struct.pack('<I', fc))
        for o in offsets:
            f.write(struct.pack('<I', o))
        for m in masks:
            f.write(m)

    total_kb = off / 1024
    avg_kb = sum(len(m) for m in masks) / fc / 1024
    print(f"  char={fw}x{fh}, pos=({x_off},{y_off})")
    print(f"  {out_path}: {fc} frames, {total_kb:.0f}KB total, avg {avg_kb:.1f}KB/frame")


if __name__ == '__main__':
    for png_dir in sys.argv[1:]:
        base = os.path.basename(os.path.normpath(png_dir))
        out = os.path.join(os.path.dirname(png_dir), f"{base}.mask")
        build_mask(png_dir, out)
