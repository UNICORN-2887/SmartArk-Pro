#!/usr/bin/env python3
"""预览纸偶转换结果：按 z 序 alpha 合成 scene.json 所有图层 → preview.png
用法：python preview.py <ppd_dir> [out.png] [--landscape]
横屏模式（--landscape）：合成后逆时针转 90° 输出 800×480 正立图（模拟用户横持观看）。
"""
import json
import os
import struct
import sys

from PIL import Image

def main():
    ppd_dir = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else 'preview.png'
    land = '--landscape' in sys.argv
    scene = json.load(open(os.path.join(ppd_dir, 'scene.json'), encoding='utf-8'))

    W, H = 480, 800
    canvas = Image.new('RGBA', (W, H), (30, 36, 48, 255))
    layers = sorted(scene['layers'], key=lambda l: l['z'])
    for ly in layers:
        raw_path = os.path.join(ppd_dir, f"{ly['name']}.raw")
        if not os.path.exists(raw_path):
            print(f"missing: {raw_path}")
            continue
        with open(raw_path, 'rb') as f:
            w, h = struct.unpack('<HH', f.read(4))
            data = f.read(w * h * 4)
        img = Image.frombytes('RGBA', (w, h), data)
        canvas.alpha_composite(img, (ly['x'], ly['y']))
    # 不加旋转：scene.json 已是设备场景坐标（y 向下、人物正立/横屏侧躺），预览与设备显示一致
    if land:
        canvas = canvas.rotate(-90, expand=True)   # 逆时针 90° → 800×480 正立（模拟横持观看）
    canvas.convert('RGB').save(out)
    print(f"preview → {out} ({len(layers)} layers{' landscape' if land else ''})")

if __name__ == '__main__':
    main()
