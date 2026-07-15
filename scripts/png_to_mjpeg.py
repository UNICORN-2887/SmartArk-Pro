#!/usr/bin/env python3
"""
将PNG序列（带透明通道）转为MJPEG文件，自动叠加红色背景
用法: python png_to_mjpeg.py <输入目录> [输出文件名]

输入目录结构:
  frames/
    neutral/
      neutral000.png
      neutral001.png
      ...
    thinking/
      thinking000.png
      ...

运行:
  python scripts/png_to_mjpeg.py frames/neutral neutral    → 生成 neutral.mjpeg
  python scripts/png_to_mjpeg.py frames/thinking thinking  → 生成 thinking.mjpeg
  python scripts/png_to_mjpeg.py frames                    → 生成所有子目录的 .mjpeg

流程: PNG(RGBA) → 叠红色背景(255,0,0) → RGB → JPEG → MJPEG打包
"""

import os, struct, sys
from PIL import Image

RED_BG = (255, 0, 0)  # 纯红色背景（与PPA色键抠图匹配）

def png_to_jpeg_bytes(png_path):
    """PNG叠红色背景 → JPEG字节"""
    img = Image.open(png_path)
    if img.mode == 'RGBA':
        bg = Image.new('RGB', img.size, RED_BG)
        bg.paste(img, mask=img.split()[3])
        img = bg
    elif img.mode != 'RGB':
        img = img.convert('RGB')
    # 缩放到480x800
    img = img.resize((480, 800), Image.LANCZOS)
    from io import BytesIO
    buf = BytesIO()
    img.save(buf, 'JPEG', quality=85)
    return buf.getvalue()

def pack_mjpeg(input_dir, output_name):
    """将一个目录的PNG序列打包为MJPEG"""
    files = sorted([f for f in os.listdir(input_dir)
                   if f.lower().endswith('.png')])
    if not files:
        print(f"  [跳过] {input_dir}: 没有PNG文件")
        return

    frames = []
    for f in files:
        path = os.path.join(input_dir, f)
        frames.append(png_to_jpeg_bytes(path))

    output_path = output_name + ".mjpeg"
    header_size = 4 + len(frames) * 4
    with open(output_path, 'wb') as out:
        out.write(struct.pack('<I', len(frames)))
        offset = header_size
        for data in frames:
            out.write(struct.pack('<I', offset))
            offset += len(data)
        for data in frames:
            out.write(data)

    mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"  → {output_path} ({len(frames)}帧, {mb:.1f}MB)")

    # 生成第一帧预览，验证红色背景效果
    preview_path = output_name + "_preview.jpg"
    preview = Image.open(os.path.join(input_dir, files[0]))
    if preview.mode == 'RGBA':
        bg = Image.new('RGB', preview.size, RED_BG)
        bg.paste(preview, mask=preview.split()[3])
        preview = bg
    preview = preview.resize((480, 800), Image.LANCZOS)
    preview.save(preview_path, 'JPEG', quality=95)
    print(f"  → {preview_path} (预览，确认红色背景效果)")

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        print("示例:")
        print("  python png_to_mjpeg.py frames/neutral     → neutral.mjpeg")
        print("  python png_to_mjpeg.py frames/thinking    → thinking.mjpeg")
        print("  python png_to_mjpeg.py frames              → 批量处理所有子目录")
        sys.exit(1)

    target = sys.argv[1]
    output_name = sys.argv[2] if len(sys.argv) > 2 else None

    if os.path.isdir(target) and output_name:
        # 单个目录: python png_to_mjpeg.py frames/neutral neutral
        pack_mjpeg(target, output_name)
    elif os.path.isdir(target):
        # 批量: python png_to_mjpeg.py frames
        for name in sorted(os.listdir(target)):
            subdir = os.path.join(target, name)
            if os.path.isdir(subdir):
                pack_mjpeg(subdir, name)
    else:
        print(f"错误: {target} 不是有效目录")

if __name__ == "__main__":
    main()
