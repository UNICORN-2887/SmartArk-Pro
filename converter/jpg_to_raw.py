#!/usr/bin/env python3
"""JPG → raw RGB565 转换，ESP32 直接读取无需解码"""
import sys, os, struct
from PIL import Image

def main():
    if len(sys.argv) < 2:
        print(f"用法: python {sys.argv[0]} <文件夹>")
        sys.exit(1)
    src = sys.argv[1]
    out = src.rstrip("/\\") + "_raw"
    os.makedirs(out, exist_ok=True)
    for f in sorted(os.listdir(src)):
        if not f.lower().endswith(('.jpg', '.jpeg', '.png')):
            continue
        img = Image.open(os.path.join(src, f)).convert("RGB")
        w, h = img.size
        pixels = list(img.getdata())
        with open(os.path.join(out, os.path.splitext(f)[0] + ".raw"), "wb") as fp:
            for r, g, b in pixels:
                # RGB888 → RGB565 little-endian
                c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                fp.write(struct.pack('<H', c))
        print(f"  {f} → {w}x{h} raw")
    print(f"\n完成 → {out}/")

if __name__ == "__main__":
    main()
