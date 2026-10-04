#!/usr/bin/env python3
"""预览纸偶转换结果：按 z 序 alpha 合成 scene.json 所有图层 → preview.png
用法：python preview.py <ppd_dir> [out.png] [--landscape] [--anim Sleep] [--t 0.99] [--only Tail]
横屏模式（--landscape）：合成后逆时针转 90° 输出 800×480 正立图（模拟用户横持观看）。
"""
import json
import math
import os
import struct
import sys

from PIL import Image

def arg_value(args, name, default=None):
    if name not in args:
        return default
    i = args.index(name)
    return args[i + 1] if i + 1 < len(args) else default

def sample_track(track, duration, t):
    if not track:
        return 0, 0, 0, 1, 1, 1, None
    pos = max(0, min(0.999999, t / max(duration, 0.001))) * len(track)
    i0 = int(pos) % len(track)
    i1 = min(i0 + 1, len(track) - 1)
    frac = pos - int(pos)
    k0, k1 = track[i0], track[i1]
    def val(k, i, default):
        return k[i] if len(k) > i else default
    mat = None
    if len(k0) >= 10 and len(k1) >= 10:
        mat = (
            val(k0, 6, 1) + (val(k1, 6, 1) - val(k0, 6, 1)) * frac,
            val(k0, 7, 0) + (val(k1, 7, 0) - val(k0, 7, 0)) * frac,
            val(k0, 8, 0) + (val(k1, 8, 0) - val(k0, 8, 0)) * frac,
            val(k0, 9, 1) + (val(k1, 9, 1) - val(k0, 9, 1)) * frac,
        )
    return (
        val(k0, 0, 0) + (val(k1, 0, 0) - val(k0, 0, 0)) * frac,
        val(k0, 1, 0) + (val(k1, 1, 0) - val(k0, 1, 0)) * frac,
        val(k0, 2, 0) + (val(k1, 2, 0) - val(k0, 2, 0)) * frac,
        val(k0, 3, 1) + (val(k1, 3, 1) - val(k0, 3, 1)) * frac,
        val(k0, 4, 1) + (val(k1, 4, 1) - val(k0, 4, 1)) * frac,
        val(k0, 5, 1) + (val(k1, 5, 1) - val(k0, 5, 1)) * frac,
        mat,
    )

def main():
    args = sys.argv[1:]
    ppd_dir = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else 'preview.png'
    land = '--landscape' in args
    anim_name = arg_value(args, '--anim')
    t_ratio = float(arg_value(args, '--t', '0.0'))
    only = arg_value(args, '--only')
    scene = json.load(open(os.path.join(ppd_dir, 'scene.json'), encoding='utf-8'))
    anim = None
    if anim_name:
        anims = json.load(open(os.path.join(ppd_dir, 'anims.json'), encoding='utf-8'))
        anim = anims[anim_name]

    W, H = 480, 800
    canvas = Image.new('RGBA', (W, H), (30, 36, 48, 255))
    layers = sorted(scene['layers'], key=lambda l: l['z'])
    for ly in layers:
        if only and only not in ly['name']:
            continue
        dx = dy = drot = 0
        vis = 1 if ly.get('visible', True) else 0
        sx = sy = 1
        mat = None
        if anim:
            track = anim['layers'].get(ly['name'])
            if track:
                dx, dy, drot, vis, sx, sy, mat = sample_track(track, anim['duration'], anim['duration'] * t_ratio)
        if vis < 0.5:
            continue
        raw_path = os.path.join(ppd_dir, f"{ly['name']}.raw")
        if not os.path.exists(raw_path):
            print(f"missing: {raw_path}")
            continue
        with open(raw_path, 'rb') as f:
            w, h = struct.unpack('<HH', f.read(4))
            data = f.read(w * h * 4)
        img = Image.frombytes('RGBA', (w, h), data)
        px = ly.get('bone_px', ly['w'] / 2)
        py = ly.get('bone_py', ly['h'] / 2)
        if mat:
            a, d, b, e = mat
        else:
            th = math.radians(drot)
            co, si = math.cos(th), math.sin(th)
            a, b = co * sx, -si * sy
            d, e = si * sx, co * sy
        c = ly['x'] + dx + px - a * px - b * py
        f = ly['y'] + dy + py - d * px - e * py
        corners = [(0, 0), (w, 0), (w, h), (0, h)]
        xs = [a * x + b * y + c for x, y in corners]
        ys = [d * x + e * y + f for x, y in corners]
        x0, y0 = math.floor(min(xs)), math.floor(min(ys))
        x1, y1 = math.ceil(max(xs)), math.ceil(max(ys))
        ow, oh = max(1, x1 - x0), max(1, y1 - y0)
        det = a * e - b * d
        if abs(det) < 1e-6:
            continue
        ia, ib = e / det, -b / det
        id_, ie = -d / det, a / det
        ic = ia * (x0 - c) + ib * (y0 - f)
        iff = id_ * (x0 - c) + ie * (y0 - f)
        img = img.transform((ow, oh), Image.Transform.AFFINE,
                            (ia, ib, ic, id_, ie, iff),
                            resample=Image.Resampling.BICUBIC)
        canvas.alpha_composite(img, (x0, y0))
    # 不加旋转：scene.json 已是设备场景坐标（y 向下、人物正立/横屏侧躺），预览与设备显示一致
    if land:
        canvas = canvas.rotate(-90, expand=True)   # 逆时针 90° → 800×480 正立（模拟横持观看）
    canvas.convert('RGB').save(out)
    print(f"preview → {out} ({len(layers)} layers{' landscape' if land else ''}{' ' + anim_name if anim_name else ''})")

if __name__ == '__main__':
    main()
