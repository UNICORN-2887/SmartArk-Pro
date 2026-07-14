#!/usr/bin/env python3
"""Convert background.png to background.jpg (480x800, RGB565-optimized)"""

from PIL import Image
import os

def main():
    input_path = "background.png"
    output_path = "background.jpg"

    if not os.path.exists(input_path):
        print(f"错误：找不到 {input_path}")
        return

    img = Image.open(input_path)
    print(f"输入: {img.size}, 模式: {img.mode}")

    # 如果是RGBA，叠到黑色背景上
    if img.mode == 'RGBA':
        bg = Image.new('RGB', img.size, (0, 0, 0))
        bg.paste(img, mask=img.split()[3])  # 用alpha通道做mask
        img = bg
        print("RGBA → RGB（黑色背景合成）")
    elif img.mode != 'RGB':
        img = img.convert('RGB')

    # 缩放到480x800（保持比例，居中裁剪）
    target = (480, 800)
    img_ratio = img.width / img.height
    target_ratio = target[0] / target[1]

    if img_ratio > target_ratio:
        new_h = target[1]
        new_w = int(img_ratio * new_h)
    else:
        new_w = target[0]
        new_h = int(new_w / img_ratio)

    img = img.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - target[0]) // 2
    top = (new_h - target[1]) // 2
    img = img.crop((left, top, left + target[0], top + target[1]))
    print(f"缩放+裁剪: {img.size}")

    # 保存为高质量JPEG
    img.save(output_path, "JPEG", quality=95)
    size_kb = os.path.getsize(output_path) / 1024
    print(f"输出: {output_path} ({size_kb:.1f} KB)")
    print("完成！放入 SD 卡根目录即可。")

if __name__ == "__main__":
    main()
