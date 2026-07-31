#!/usr/bin/env python3
"""PNG序列 → MJPEG(红底) + .mask(RLE遮罩) 一键生成"""
import struct, io, sys, os
import numpy as np
from PIL import Image

W, H = 480, 800
RED = (255, 0, 0)
MARGIN_BOTTOM = 60


def rle_encode(mask_flat):
    out = bytearray()
    i, n = 0, len(mask_flat)
    while i < n:
        val = mask_flat[i]
        cnt = 1
        while i + cnt < n and mask_flat[i + cnt] == val and cnt < 65535:
            cnt += 1
        out += struct.pack('<HB', cnt, val)
        i += cnt
    return bytes(out)


def build(png_dir, out_mjpeg, quality=85, with_mask=True):
    files = sorted([f for f in os.listdir(png_dir) if f.lower().endswith('.png')])
    if not files:
        print(f"No PNGs in {png_dir}"); return

    first = Image.open(os.path.join(png_dir, files[0]))
    fw, fh = first.size
    x_off = (W - fw) // 2
    y_off = H - fh - MARGIN_BOTTOM
    print(f"{len(files)} frames, char={fw}x{fh}, pos=({x_off},{y_off}), Q={quality}")

    frames, masks = [], []
    for fn in files:
        img = Image.open(os.path.join(png_dir, fn))
        if img.mode != 'RGBA':
            img = img.convert('RGBA')

        # MJPEG 帧（红底）
        canvas = Image.new('RGB', (W, H), RED)
        canvas.paste(img, (x_off, y_off), img)
        buf = io.BytesIO()
        canvas.save(buf, format='JPEG', quality=quality)
        frames.append(buf.getvalue())

        # 遮罩（对齐画布位置）
        if with_mask:
            arr = np.array(img)
            alpha = arr[:, :, 3]
            char_mask = (alpha > 128).astype(np.uint8)
            full_mask = np.zeros((H, W), dtype=np.uint8)
            sy1, sy2 = max(0, -y_off), min(fh, H - y_off)
            dy1, dy2 = max(0, y_off), min(H, y_off + fh)
            sx1, sx2 = max(0, -x_off), min(fw, W - x_off)
            dx1, dx2 = max(0, x_off), min(W, x_off + fw)
            h, w = sy2 - sy1, sx2 - sx1
            if h > 0 and w > 0:
                full_mask[dy1:dy1+h, dx1:dx1+w] = char_mask[sy1:sy1+h, sx1:sx1+w]
            masks.append(rle_encode(full_mask.flatten()))

    # 写 MJPEG
    fc = len(frames)
    with open(out_mjpeg, 'wb') as f:
        f.write(struct.pack('<I', fc))
        off = 4 + fc * 4
        for jpg in frames:
            f.write(struct.pack('<I', off)); off += len(jpg)
        for jpg in frames:
            f.write(jpg)
    print(f"  → {out_mjpeg}: {fc}f, {off/1024:.0f}KB")

    # 写 MASK
    if with_mask:
        out_mask = out_mjpeg.replace('.mjpeg', '.mask')
        header = 4 + fc * 4
        with open(out_mask, 'wb') as f:
            f.write(struct.pack('<I', fc))
            off = header
            for m in masks:
                f.write(struct.pack('<I', off)); off += len(m)
            for m in masks:
                f.write(m)
        print(f"  → {out_mask}: {off/1024:.0f}KB (avg {sum(len(m) for m in masks)/fc/1024:.1f}KB/frame)")


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description='PNG→MJPEG+Mask')
    p.add_argument('png_dir', help='PNG序列目录')
    p.add_argument('out_mjpeg', help='输出.mjpeg路径')
    p.add_argument('-q', '--quality', type=int, default=85, help='JPEG质量(默认85)')
    p.add_argument('--no-mask', action='store_true', help='不生成.mask文件')
    args = p.parse_args()
    build(args.png_dir, args.out_mjpeg, args.quality, not args.no_mask)
