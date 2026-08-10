"""PNG → RGB565 raw binary converter for P4 Live2D texture loader.
Usage: python png2r5g6b5.py input.png output.raw"""
import sys, struct
from PIL import Image

src = sys.argv[1] if len(sys.argv) > 1 else "texture_00.png"
dst = sys.argv[2] if len(sys.argv) > 2 else src.replace(".png", ".raw")

img = Image.open(src).convert("RGBA")
w, h = img.size
with open(dst, "wb") as f:
    f.write(struct.pack("<HH", w, h))
    for y in range(h):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            f.write(struct.pack("<H", rgb565))
print(f"Saved {dst}: {w}x{h} RGB565 ({os.path.getsize(dst)/1024:.0f}KB)")
