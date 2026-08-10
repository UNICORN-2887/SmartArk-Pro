"""PNG sequence → MJPEG + RLE Mask converter (for character expressions/emoji).

Usage:
  python png2emoji.py <input_root> [--fps 25] [--max-frames 120]
  python png2emoji.py <input_root> --single <emotion_name>

Directory structure (multi):
  input_root/
    neutral/    neutral000.png  neutral001.png ...
    happy/      happy000.png    happy001.png ...
    angry/      ...

  Output per emotion:
    input_root/neutral.mjpeg  +  neutral.mask
    input_root/happy.mjpeg    +  happy.mask
    ...

Directory structure (--single):
  input_root/*.png  →  input_root/<name>.mjpeg + <name>.mask"""
import os, struct, sys, glob, math
from PIL import Image

# ── Helpers ──
def fail(msg):
    print(f"[FAIL] {msg}")
    sys.exit(1)

def rle_encode_pixels(pixels, w, h, threshold=128):
    """Encode alpha values into RLE: [2B count][1B value]... value=0 transparent, 1 opaque."""
    total = w * h
    runs = []
    i = 0
    while i < total:
        alpha = pixels[i][3] if isinstance(pixels[i], tuple) and len(pixels[i]) > 3 else 255
        val = 1 if alpha >= threshold else 0
        cnt = 1
        while i + cnt < total and cnt < 65535:
            na = pixels[i + cnt][3] if isinstance(pixels[i + cnt], tuple) and len(pixels[i + cnt]) > 3 else 255
            nv = 1 if na >= threshold else 0
            if nv != val:
                break
            cnt += 1
        runs.append((cnt, val))
        i += cnt
    return runs

def build_mjpeg(frame_data_list, output_path):
    fc = len(frame_data_list)
    header = bytearray(4 + fc * 4)
    struct.pack_into('<I', header, 0, fc)
    offset = 4 + fc * 4
    body = bytearray()
    for i, data in enumerate(frame_data_list):
        struct.pack_into('<I', header, 4 + i * 4, offset)
        body.extend(data)
        offset += len(data)
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(body)
    return fc

def build_mask(rle_frames, output_path):
    """Write RLE mask file: [4B fc][N×4B offsets][RLE data...]"""
    fc = len(rle_frames)
    header_size = 4 + fc * 4
    body = bytearray()
    offsets = []
    for runs in rle_frames:
        offsets.append(header_size + len(body))
        for cnt, val in runs:
            body.extend(struct.pack('<H', cnt))
            body.append(val)
    with open(output_path, 'wb') as f:
        f.write(struct.pack('<I', fc))
        for off in offsets:
            f.write(struct.pack('<I', off))
        f.write(body)
    return fc

# ── Process a single emotion folder ──
def process_emotion(folder, output_name=None, fps=25, max_frames=120, dest=None):
    name = output_name or os.path.basename(os.path.normpath(folder))
    # Collect PNGs
    pngs = []
    for ext in ('*.png', '*.PNG'):
        pngs.extend(glob.glob(os.path.join(folder, ext)))
    pngs = sorted(set(pngs))

    if not pngs:
        print(f"  [WARN] No PNGs in {folder}")
        return False

    print(f"  {name}: {len(pngs)} PNGs")

    # Frame cap
    if len(pngs) > max_frames:
        step = len(pngs) / max_frames
        pngs = [pngs[int(i * step)] for i in range(max_frames)]
        print(f"    capped to {len(pngs)}")

    jpg_data = []
    rle_frames = []

    for p in pngs:
        img = Image.open(p).convert('RGBA')
        w, h = img.size
        pixels = list(img.getdata())

        # RLE mask from alpha
        runs = rle_encode_pixels(pixels, w, h)
        rle_frames.append(runs)

        # JPEG: flatten on white background
        bg = Image.new('RGB', (w, h), (255, 255, 255))
        bg.paste(img, mask=img.split()[3])
        import io
        buf = io.BytesIO()
        bg.save(buf, 'JPEG', quality=85)
        jpg_data.append(buf.getvalue())

    # Write outputs (to --dest if given, else source folder)
    out_dir = dest if dest else folder
    os.makedirs(out_dir, exist_ok=True)
    mjpeg_path = os.path.join(out_dir, f"{name}.mjpeg")
    mask_path = os.path.join(out_dir, f"{name}.mask")

    fc1 = build_mjpeg(jpg_data, mjpeg_path)
    fc2 = build_mask(rle_frames, mask_path)

    sz_mj = os.path.getsize(mjpeg_path) / 1024
    sz_msk = os.path.getsize(mask_path) / 1024
    print(f"    {name}.mjpeg: {fc1}f, {sz_mj:.0f}KB | {name}.mask: {sz_msk:.0f}KB")
    return True

# ── Main ──
if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    root = sys.argv[1]
    fps = 25
    max_frames = 120
    single_mode = None
    dest = None

    for i, arg in enumerate(sys.argv):
        if arg == '--fps' and i + 1 < len(sys.argv):
            fps = int(sys.argv[i + 1])
        if arg == '--max-frames' and i + 1 < len(sys.argv):
            max_frames = int(sys.argv[i + 1])
        if arg == '--single' and i + 1 < len(sys.argv):
            single_mode = sys.argv[i + 1]
        if arg == '--dest' and i + 1 < len(sys.argv):
            dest = sys.argv[i + 1]

    if single_mode:
        process_emotion(root, output_name=single_mode, fps=fps, max_frames=max_frames, dest=dest)
    else:
        subdirs = sorted([d for d in os.listdir(root)
                          if os.path.isdir(os.path.join(root, d)) and not d.startswith('.')])
        if not subdirs:
            process_emotion(root, fps=fps, max_frames=max_frames, dest=dest)
        else:
            ok = 0
            for sd in subdirs:
                if process_emotion(os.path.join(root, sd), fps=fps, max_frames=max_frames, dest=dest):
                    ok += 1
            print(f"\nDone: {ok}/{len(subdirs)} emotions processed")

    print("\n→ Copy *.mjpeg + *.mask files to SD card agent's emoji/ folder")
