#!/usr/bin/env python3
"""纯 JPG 序列 → MJPEG 打包（不做任何合成/叠加）"""
import os, sys, struct

def jpg_to_mjpeg(src_dir, out_path):
    files = sorted([f for f in os.listdir(src_dir) if f.lower().endswith('.jpg')])
    if not files:
        print(f"  0 frames, skip"); return 0

    buffers = []
    for f in files:
        with open(os.path.join(src_dir, f), 'rb') as fh:
            buffers.append(fh.read())

    frame_count = len(buffers)
    header_size = 4 + frame_count * 4
    offset = header_size

    with open(out_path, 'wb') as f:
        f.write(struct.pack('<I', frame_count))
        for buf in buffers:
            f.write(struct.pack('<I', offset))
            offset += len(buf)
        for buf in buffers:
            f.write(buf)

    kb = offset / 1024
    print(f"  {out_path}: {frame_count}f, {kb:.0f}KB")
    return frame_count

if __name__ == '__main__':
    tasks = [
        (r"F:\main\operator\CASTER\5STAR\Amiya\cover",    r"阿米娅\cover.mjpeg"),
        (r"F:\main\operator\MEDIC\6STAR\Kaltsit\cover",   r"凯尔希\cover.mjpeg"),
    ]
    os.makedirs("cover_output", exist_ok=True)
    for src, name in tasks:
        print(f"[{name.split(chr(92))[0]}]")
        jpg_to_mjpeg(src, os.path.join("cover_output", os.path.basename(name)))
    print("\nDone → cover_output/")
