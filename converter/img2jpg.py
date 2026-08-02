#!/usr/bin/env python3
"""Image -> 480x800 JPG.  Run this script in the folder containing your image."""

import os, sys, subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__)) if '__file__' in dir() else os.getcwd()
OUT = os.path.join(SCRIPT_DIR, 'Profile.jpg')
EXTS = ('.png', '.bmp', '.webp', '.gif', '.tiff', '.tif', '.jpeg', '.jpg')

def fail(msg):
    print(f'\n[FAIL] {msg}')
    input('\nPress Enter to exit...')
    sys.exit(1)

print('=' * 50)
print('  Image -> 480x800 JPG Converter')
print('=' * 50)

# Check / install Pillow
try:
    from PIL import Image
except ImportError:
    print('\nInstalling Pillow ...')
    result = subprocess.run([sys.executable, '-m', 'pip', 'install', 'Pillow', '-q'],
                          capture_output=True)
    if result.returncode != 0:
        fail('Failed to install Pillow. Run: pip install Pillow')
    from PIL import Image
    print('   Pillow installed successfully')

# Find source image
src = None
for fn in sorted(os.listdir(SCRIPT_DIR)):
    low = fn.lower()
    if low.endswith(EXTS) and low != 'profile.jpg':
        src = os.path.join(SCRIPT_DIR, fn)
        break

if src is None:
    print(f'\nFiles in current folder:')
    for f in sorted(os.listdir(SCRIPT_DIR)):
        print(f'  {f}')
    fail('No image found. Supported: PNG, BMP, WEBP, GIF, TIFF, JPEG')

print(f'\nConverting: {os.path.basename(src)} -> Profile.jpg (480x800) ...')

try:
    img = Image.open(src).convert('RGBA')
    w, h = img.size
    if w > h:
        img = img.transpose(Image.ROTATE_270)  # 横屏→竖屏
        print(f'   Rotated {w}x{h} → {img.size[0]}x{img.size[1]}')
    bg = Image.new('RGB', img.size, (255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    bg = bg.resize((480, 800), Image.LANCZOS)
    bg.save(OUT, 'JPEG', quality=92)
    print(f'   Profile.jpg: {bg.size[0]}x{bg.size[1]}, {os.path.getsize(OUT)/1024:.0f} KB')
except Exception as e:
    fail(f'Conversion failed: {e}')

print(f'\n{"=" * 50}')
print(f'  DONE! Profile.jpg is ready.')
print(f'  Insert SD card into device, tap the button to view.')
print(f'{"=" * 50}')
input('\nPress Enter to exit...')
