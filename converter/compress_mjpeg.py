#!/usr/bin/env python3
"""降 MJPEG 的 JPEG 质量 → 减小文件体积"""
import struct, io, sys, os
from PIL import Image

QUALITY = 40  # JPEG 质量 (1-100)，越低越小

def compress(src, dst=None, quality=QUALITY):
    if dst is None:
        base, ext = os.path.splitext(src)
        dst = f"{base}_lq{ext}"

    with open(src, 'rb') as f:
        data = f.read()

    fc = struct.unpack('<I', data[0:4])[0]
    frames = []
    for i in range(fc):
        off = struct.unpack('<I', data[4+i*4:8+i*4])[0]
        end = struct.unpack('<I', data[8+i*4:12+i*4])[0] if i+1 < fc else len(data)
        frames.append(data[off:end])

    print(f"{os.path.basename(src)}: {fc} 帧, {len(data)/1024/1024:.1f}MB → 重压中...")

    new_frames = []
    for jpg in frames:
        img = Image.open(io.BytesIO(jpg))
        buf = io.BytesIO()
        img.save(buf, format='JPEG', quality=quality)
        new_frames.append(buf.getvalue())

    with open(dst, 'wb') as f:
        f.write(struct.pack('<I', fc))
        off = 4 + fc * 4
        for nf in new_frames:
            f.write(struct.pack('<I', off))
            off += len(nf)
        for nf in new_frames:
            f.write(nf)

    print(f"  → {os.path.basename(dst)}: {off/1024/1024:.1f}MB (quality={quality})")

if __name__ == '__main__':
    for src in sys.argv[1:]:
        compress(src)
