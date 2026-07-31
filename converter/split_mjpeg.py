#!/usr/bin/env python3
"""拆分 MJPEG 为 N 个 part，用于滚动预加载"""
import struct, sys, os

def split_mjpeg(src, out_dir, parts=4):
    with open(src, 'rb') as f:
        data = f.read()
    fc = struct.unpack('<I', data[0:4])[0]
    offsets = [struct.unpack('<I', data[4+i*4:8+i*4])[0] for i in range(fc)]

    name = os.path.splitext(os.path.basename(src))[0]
    per_part = (fc + parts - 1) // parts

    for p in range(parts):
        start_idx = p * per_part
        end_idx = min(start_idx + per_part, fc)
        if start_idx >= fc: break

        part_fc = end_idx - start_idx
        first_offset = offsets[start_idx]

        # 提取这部分的所有帧
        frames = []
        for i in range(start_idx, end_idx):
            begin = offsets[i]
            end = offsets[i+1] if i+1 < fc else len(data)
            frames.append(data[begin:end])

        # 写入 part 文件
        out_path = os.path.join(out_dir, f"{name}_part{p}.mjpeg")
        with open(out_path, 'wb') as f:
            f.write(struct.pack('<I', part_fc))
            off = 4 + part_fc * 4
            for jpg in frames:
                f.write(struct.pack('<I', off))
                off += len(jpg)
            for jpg in frames:
                f.write(jpg)

        kb = off / 1024
        print(f"  part{p}: frames {start_idx}-{end_idx-1} ({part_fc}f), {kb:.0f}KB → {out_path}")

if __name__ == '__main__':
    for src in sys.argv[1:]:
        print(f"[{os.path.basename(src)}]")
        split_mjpeg(src, os.path.dirname(src))
