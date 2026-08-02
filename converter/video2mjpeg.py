#!/usr/bin/env python3
"""MP4 -> MJPEG (480x800, 30fps).  Run this script in the folder containing Profile.mp4."""

import os, sys, struct, subprocess, shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__)) if '__file__' in dir() else os.getcwd()
SRC = os.path.join(SCRIPT_DIR, 'Profile.mp4')
OUT = os.path.join(SCRIPT_DIR, 'Profile.mjpeg')
TMP = os.path.join(SCRIPT_DIR, '_mjpeg_temp')

def fail(msg):
    print(f'\n[FAIL] {msg}')
    if os.path.isdir(TMP):
        shutil.rmtree(TMP, ignore_errors=True)
    input('\nPress Enter to exit...')
    sys.exit(1)

print('=' * 50)
print('  MP4 -> MJPEG Converter (480x800, 25fps, max 10s/200 frames)')
print('=' * 50)

# Check ffmpeg
try:
    subprocess.run(['ffmpeg', '-version'], capture_output=True, check=True)
except (FileNotFoundError, subprocess.CalledProcessError):
    fail('ffmpeg not found. Install from https://ffmpeg.org/download.html')

if not os.path.isfile(SRC):
    print(f'\nFiles in current folder:')
    for f in sorted(os.listdir(SCRIPT_DIR)):
        print(f'  {f}')
    fail(f'Profile.mp4 not found in {SCRIPT_DIR}')

FPS = 25          # 目标帧率
MAX_SEC = 10      # 最多取前 10 秒
MAX_FRAMES = 200  # PPA 硬限制（MAX_CACHE=200）

print(f'\n[1/3] Extracting frames (25fps, max {MAX_SEC}s, {MAX_FRAMES} frames cap) ...')
if os.path.isdir(TMP):
    shutil.rmtree(TMP)
os.makedirs(TMP)

cmd = [
    'ffmpeg', '-i', SRC,
    '-t', str(MAX_SEC),  # 超长视频只取前 N 秒
    '-vf', 'scale=480:800:force_original_aspect_ratio=decrease,pad=480:800:(ow-iw)/2:(oh-ih)/2:color=black',
    '-q:v', '3', '-r', str(FPS),
    os.path.join(TMP, '%04d.jpg'),
    '-hide_banner', '-loglevel', 'error'
]
result = subprocess.run(cmd, capture_output=True)
if result.returncode != 0:
    err = result.stderr.decode('utf-8', errors='replace').strip()
    fail(f'ffmpeg failed:\n{err}')

files = sorted([f for f in os.listdir(TMP) if f.lower().endswith('.jpg')])
if not files:
    fail('No frames extracted (video might be empty)')
print(f'   {len(files)} frames extracted ({FPS}fps × {MAX_SEC}s limit)')

# 二次兜底：超过上限则均匀采样降帧
if len(files) > MAX_FRAMES:
    step = len(files) / MAX_FRAMES
    sampled = [files[int(i * step)] for i in range(MAX_FRAMES)]
    for f in files:
        if f not in sampled:
            os.remove(os.path.join(TMP, f))
    files = sampled
    print(f'   Trimmed to {len(files)} frames ({MAX_FRAMES} max)')

print(f'\n[2/3] Building MJPEG ...')
with open(OUT, 'wb') as f:
    f.write(struct.pack('<I', len(files)))
    offset = 4 + len(files) * 4
    offsets = []
    for fn in files:
        data = open(os.path.join(TMP, fn), 'rb').read()
        offsets.append(offset)
        offset += len(data)
    for o in offsets:
        f.write(struct.pack('<I', o))
    for fn in files:
        f.write(open(os.path.join(TMP, fn), 'rb').read())

size_kb = os.path.getsize(OUT) / 1024
print(f'   Profile.mjpeg: {len(files)} frames, {size_kb:.0f} KB')

print(f'\n[3/3] Cleaning up ...')
shutil.rmtree(TMP)
print(f'\n{"=" * 50}')
print(f'  DONE! Profile.mjpeg is ready.')
print(f'  Insert SD card into device, tap the button to view.')
print(f'{"=" * 50}')
input('\nPress Enter to exit...')
