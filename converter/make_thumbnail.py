"""Image → 108×228 JPG thumbnail for character index page.
Usage: python make_thumbnail.py <input_image> [output_name]
Example: python make_thumbnail.py Civilight_Eterna.png"""
import os, sys
from PIL import Image

if len(sys.argv) < 2:
    print(__doc__)
    sys.exit(1)

src = sys.argv[1]
if not os.path.exists(src):
    print(f"[FAIL] File not found: {src}")
    sys.exit(1)

# Output name: same basename, .jpg extension
base = os.path.splitext(os.path.basename(src))[0]
out = sys.argv[2] if len(sys.argv) > 2 else f"{base}.jpg"
out_dir = os.path.dirname(os.path.abspath(src))
out_path = os.path.join(out_dir, out)

img = Image.open(src).convert('RGBA')
w, h = img.size
print(f"Input: {w}×{h}")

# Fill transparent areas with white
bg = Image.new('RGB', img.size, (255, 255, 255))
bg.paste(img, mask=img.split()[3])

# Resize to 108×228, preserve aspect ratio with padding
target_w, target_h = 108, 228
ratio = min(target_w / w, target_h / h)
new_w, new_h = int(w * ratio), int(h * ratio)
resized = bg.resize((new_w, new_h), Image.LANCZOS)

# Center on 108×228 canvas
canvas = Image.new('RGB', (target_w, target_h), (255, 255, 255))
offset_x = (target_w - new_w) // 2
offset_y = (target_h - new_h) // 2
canvas.paste(resized, (offset_x, offset_y))

canvas.save(out_path, 'JPEG', quality=92)
print(f"[OK] {out_path} ({target_w}×{target_h}, {os.path.getsize(out_path)/1024:.0f}KB)")
print(f"  → Copy to /sdcard/main/operator/INDEX/{{职业}}_108x228/{{星级}}/")
