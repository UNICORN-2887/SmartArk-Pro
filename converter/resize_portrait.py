#!/usr/bin/env python3
"""
批量转换角色立绘：PNG → JPG，统一缩放为 108×228 像素
用法：python resize_portrait.py <输入文件夹>
输出：在输入文件夹同目录创建 <foldername>_108x228 文件夹
"""

import sys, os
from PIL import Image

TARGET_W, TARGET_H = 108, 228

def main():
    if len(sys.argv) < 2:
        print(f"用法: python {sys.argv[0]} <文件夹路径>")
        sys.exit(1)

    src_dir = sys.argv[1]
    if not os.path.isdir(src_dir):
        print(f"错误: '{src_dir}' 不是文件夹")
        sys.exit(1)

    parent = os.path.dirname(src_dir.rstrip("/\\"))
    folder_name = os.path.basename(src_dir.rstrip("/\\"))
    out_dir = os.path.join(parent, f"{folder_name}_108x228") if parent else f"{folder_name}_108x228"
    os.makedirs(out_dir, exist_ok=True)

    converted = 0
    for fname in sorted(os.listdir(src_dir)):
        if not fname.lower().endswith(".png"):
            continue

        in_path = os.path.join(src_dir, fname)
        out_name = os.path.splitext(fname)[0] + ".jpg"
        out_path = os.path.join(out_dir, out_name)

        try:
            img = Image.open(in_path).convert("RGBA")
            # 白色背景，处理透明通道
            bg = Image.new("RGB", img.size, (255, 255, 255))
            bg.paste(img, mask=img.split()[3])
            # 缩放到目标尺寸（Lanczos 高质量）
            bg = bg.resize((TARGET_W, TARGET_H), Image.LANCZOS)
            bg.save(out_path, "JPEG", quality=92)
            converted += 1
            print(f"  ✓ {fname} → {out_name}")
        except Exception as e:
            print(f"  ✗ {fname}: {e}")

    print(f"\n完成: {converted} 张图片 → {out_dir}")
    print(f"尺寸: {TARGET_W}×{TARGET_H}")

if __name__ == "__main__":
    main()
