"""JPG sequence → MJPEG cover converter. Auto-resizes to target resolution.
Usage: python jpg2mjpeg.py <input_folder> [output_name] [--width 480] [--height 800] [--fps 25] [--max-frames 260]
Example: python jpg2mjpeg.py "E:\source\Civilight_Eterna\jpg" civilight_eterna_cover"""
import os, struct, sys, glob, io
from PIL import Image

def fail(msg):
    print(f"[FAIL] {msg}")
    sys.exit(1)

# ── Args ──
if len(sys.argv) < 2:
    print(__doc__)
    sys.exit(1)

src_dir = sys.argv[1]
out_name = None
target_w, target_h = 480, 800
fps = 25
max_frames = 260

for i, arg in enumerate(sys.argv):
    if arg == '--width' and i + 1 < len(sys.argv):
        target_w = int(sys.argv[i + 1])
    elif arg == '--height' and i + 1 < len(sys.argv):
        target_h = int(sys.argv[i + 1])
    elif arg == '--fps' and i + 1 < len(sys.argv):
        fps = int(sys.argv[i + 1])
    elif arg == '--max-frames' and i + 1 < len(sys.argv):
        max_frames = int(sys.argv[i + 1])
    elif not arg.startswith('--') and i >= 2:
        out_name = arg

if out_name is None:
    out_name = os.path.basename(os.path.normpath(src_dir))

# ── Collect JPGs ──
imgs = []
for ext in ('*.jpg', '*.jpeg', '*.JPG', '*.JPEG'):
    imgs.extend(glob.glob(os.path.join(src_dir, ext)))
imgs = sorted(set(imgs))

if not imgs:
    fail(f"No JPG files found in {src_dir}")

print(f"Found {len(imgs)} JPG files, target {target_w}x{target_h}")

# ── Frame cap ──
if len(imgs) > max_frames:
    step = len(imgs) / max_frames
    sampled = [imgs[int(i * step)] for i in range(max_frames)]
    print(f"Capped: {len(imgs)} → {len(sampled)} frames (max {max_frames})")
    imgs = sampled

# ── Build MJPEG ──
fc = len(imgs)
header_size = 4 + fc * 4
offsets = []
frame_data = bytearray()

for f in imgs:
    img = Image.open(f).convert('RGB')
    if img.size != (target_w, target_h):
        img = img.resize((target_w, target_h), Image.LANCZOS)

    buf = io.BytesIO()
    img.save(buf, 'JPEG', quality=90)
    jpg_bytes = buf.getvalue()

    offsets.append(header_size + len(frame_data))
    frame_data.extend(jpg_bytes)

out_path = os.path.join(src_dir, f"{out_name}.mjpeg")
with open(out_path, 'wb') as fh:
    fh.write(struct.pack('<I', fc))
    for off in offsets:
        fh.write(struct.pack('<I', off))
    fh.write(frame_data)

sz_kb = os.path.getsize(out_path) / 1024
print(f"[OK] {out_name}.mjpeg: {fc} frames, {sz_kb:.0f} KB (resized to {target_w}x{target_h})")
print(f"  → Copy to SD card agent's cover/ folder")
