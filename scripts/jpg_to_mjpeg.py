#!/usr/bin/env python3
"""将JPG序列打包为MJPEG（头部: 帧数+偏移表 | 数据: JPEG连续拼接）"""

import os, struct, sys

def pack_mjpeg(input_dir, output_path, prefix="happy"):
    files = sorted([f for f in os.listdir(input_dir) if f.startswith(prefix) and f.lower().endswith('.jpg')])
    if not files:
        print(f"错误: 在 {input_dir} 中没有找到以 {prefix} 开头的JPG文件")
        return

    # 先读取所有帧数据，计算偏移
    frames = []
    for f in files:
        path = os.path.join(input_dir, f)
        with open(path, 'rb') as jpg:
            frames.append(jpg.read())

    # 写入: [frame_count][offset_0][offset_1]...[frame_data...]
    header_size = 4 + len(frames) * 4
    with open(output_path, 'wb') as out:
        out.write(struct.pack('<I', len(frames)))  # 帧数
        offset = header_size
        for data in frames:
            out.write(struct.pack('<I', offset))    # 帧偏移
            offset += len(data)
        for data in frames:
            out.write(data)                          # JPEG数据

    total_mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"生成: {output_path}  ({len(frames)}帧, {total_mb:.1f}MB)")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("用法: python jpg_to_mjpeg.py <JPG目录> <表情名>")
        sys.exit(1)
    pack_mjpeg(sys.argv[1], sys.argv[2] + ".mjpeg", sys.argv[2])
