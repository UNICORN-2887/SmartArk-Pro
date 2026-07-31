#!/usr/bin/env python3
"""
MJPEG Converter — 透明背景PNG → 红底MJPEG（供PPA硬件色键抠图）
用法: python build_mjpeg.py <源目录> <输出目录> [--bottom-margin 60] [--jpeg-quality 85]

源目录结构:
  源目录/
    neutral/
      neutral000.png ... neutral123.png
    happy/
      happy000.png ... happy123.png
    ...

输出目录:
  输出目录/
    neutral.mjpeg
    happy.mjpeg
    ...
"""

import os, sys, struct, io, argparse
from PIL import Image
import numpy as np

DISPLAY_W = 480
DISPLAY_H = 800
# PPA color-key range: R∈[200,255], G∈[0,80], B∈[0,80]
RED_BG = (255, 0, 0)


def convert_expression(src_dir, out_path, bottom_margin=60, jpeg_quality=85):
    """将一个表情文件夹的所有PNG帧转换为MJPEG文件"""
    files = sorted([f for f in os.listdir(src_dir) if f.lower().endswith('.png')])
    if not files:
        print(f"  ⚠ 0 frames found, skipping")
        return 0

    # 先读第一帧确定尺寸和定位
    first = Image.open(os.path.join(src_dir, files[0]))
    if first.mode != 'RGBA':
        first = first.convert('RGBA')
    fw, fh = first.size

    # 水平居中，底部留边距
    x_offset = (DISPLAY_W - fw) // 2
    y_offset = DISPLAY_H - fh - bottom_margin

    print(f"  {len(files)} frames, char={fw}x{fh}, pos=({x_offset},{y_offset}), quality={jpeg_quality}")

    jpeg_buffers = []
    offsets = []

    for fname in files:
        img = Image.open(os.path.join(src_dir, fname))
        if img.mode != 'RGBA':
            img = img.convert('RGBA')

        # 红底画布
        canvas = Image.new('RGB', (DISPLAY_W, DISPLAY_H), RED_BG)
        canvas.paste(img, (x_offset, y_offset), img)

        # JPEG 编码
        buf = io.BytesIO()
        canvas.save(buf, format='JPEG', quality=jpeg_quality)
        jpeg_buffers.append(buf.getvalue())

    # 写 MJPEG 文件
    frame_count = len(jpeg_buffers)
    offset_base = 4 + frame_count * 4  # 头部大小
    current_offset = offset_base

    with open(out_path, 'wb') as f:
        f.write(struct.pack('<I', frame_count))
        for jpg in jpeg_buffers:
            f.write(struct.pack('<I', current_offset))
            current_offset += len(jpg)
        for jpg in jpeg_buffers:
            f.write(jpg)

    total_kb = current_offset / 1024
    print(f"  → {out_path} : {frame_count} frames, {total_kb:.0f} KB")
    return frame_count


def main():
    parser = argparse.ArgumentParser(description='Convert PNG frames to MJPEG for PPA hardware compositing')
    parser.add_argument('src', help='Source directory with expression subfolders')
    parser.add_argument('out', help='Output directory for .mjpeg files')
    parser.add_argument('--bottom-margin', type=int, default=60, help='Bottom margin in pixels (default: 60)')
    parser.add_argument('--quality', type=int, default=85, help='JPEG quality 1-100 (default: 85)')
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

    # 找所有子文件夹（表情）
    expressions = [
        d for d in os.listdir(args.src)
        if os.path.isdir(os.path.join(args.src, d))
    ]
    expressions.sort()

    total = 0
    for expr in expressions:
        print(f"[{expr}]")
        src_dir = os.path.join(args.src, expr)
        out_path = os.path.join(args.out, f"{expr}.mjpeg")
        n = convert_expression(src_dir, out_path, args.bottom_margin, args.quality)
        total += n

    print(f"\nDone: {len(expressions)} expressions, {total} total frames → {args.out}")


if __name__ == '__main__':
    main()
