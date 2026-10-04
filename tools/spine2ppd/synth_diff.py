#!/usr/bin/env python3
"""合成 diff 验证：复现设备端纸偶渲染数学（scene.json + anims.json 增量）合成帧，
对照官方 spine-canvas 渲染帧（render_anim_frames.mjs 输出）逐像素 diff。

设备端数学（lv2_paperdoll.c draw_band）：
  at = (t - t0) * speed；env = sin(pi*at/dur)（单次动画包络）
  pos = at/dur*(nk-1) 线性插值 → adx/ady/adrot
  层纹理绕 (x+w/2+adx, y+h/2+ady) 视觉顺时针转 adrot（mat_R={c,s,-s,c}，
  y 向下屏幕系 → PIL 逆时针需取负）；vis<0.5 跳过层。
用法: python synth_diff.py <scene_dir> <frames_dir> <anim>
输出: 每帧 mean(|dR|+|dG|+|dB|)/3 + 整体平均（与历史数字同口径）
"""
import json, struct, sys, os, math
import numpy as np
from PIL import Image

def load_raw(path):
    with open(path, 'rb') as f:
        w, h = struct.unpack('<HH', f.read(4))
        return Image.frombytes('RGBA', (w, h), f.read())

def synth_frame(scene, anim, at, raw_imgs, W=480, H=800):
    img = Image.new('RGBA', (W, H), (30, 36, 48, 255))   # 官方底色 #1e2430
    env = math.sin(math.pi * at / anim['duration']) if not anim['loop'] else 1.0
    for L in scene['layers']:
        name = L['name']
        keys = anim['layers'].get(name)
        if keys:
            pos = at / anim['duration'] * (len(keys) - 1)
            i0 = int(pos)
            if i0 >= len(keys) - 1: i0 = len(keys) - 2
            if i0 < 0: i0 = 0
            frac = pos - i0
            k0, k1 = keys[i0], keys[i0 + 1]
            adx = (k0[0] + (k1[0] - k0[0]) * frac) * env
            ady = (k0[1] + (k1[1] - k0[1]) * frac) * env
            adrot = (k0[2] + (k1[2] - k0[2]) * frac) * env
            vis = (k0[3] + (k1[3] - k0[3]) * frac) >= 0.5
        else:
            adx = ady = adrot = 0.0
            vis = L.get('visible', True)
        if not vis: continue
        im = raw_imgs.get(name)
        if im is None: continue
        x, y, w, h = L['x'], L['y'], L['w'], L['h']
        if abs(adrot) > 0.01:
            # 设备端绕 (x+w/2+adx, y+h/2+ady) 视觉顺时针转 adrot → PIL 正角逆时针取负
            r = im.rotate(-adrot, resample=Image.BILINEAR, center=(w / 2 + adx, h / 2 + ady))
            img.paste(r, (x, y), r)
        else:
            img.paste(im, (x + round(adx), y + round(ady)), im)
    return img

def diff(a, b):
    da = np.asarray(a.convert('RGB'), dtype=np.int16)
    db = np.asarray(b.convert('RGB'), dtype=np.int16)
    return float(np.abs(da - db).mean())

def main():
    scene_dir, frames_dir, anim_name = sys.argv[1], sys.argv[2], sys.argv[3]
    scene = json.load(open(os.path.join(scene_dir, 'scene.json'), encoding='utf-8'))
    anims = json.load(open(os.path.join(scene_dir, 'anims.json'), encoding='utf-8'))
    anim = anims.get(anim_name)
    if not anim:
        print(f'动画不存在: {anim_name}（有: {", ".join(anims.keys())}）')
        return
    raw_imgs = {}
    for L in scene['layers']:
        p = os.path.join(scene_dir, L['name'] + '.raw')
        if os.path.exists(p):
            raw_imgs[L['name']] = load_raw(p)
    nk = len(next(iter(anim['layers'].values())))
    n = max(8, min(80, round(anim['duration'] * 20)))
    tot = 0.0
    for f in range(n):
        ref = Image.open(os.path.join(frames_dir, f'{anim_name}_f{f}.png')).convert('RGBA')
        at = anim['duration'] * f / n
        syn = synth_frame(scene, anim, at, raw_imgs)
        d = diff(syn, ref)
        tot += d
        print(f'  f{f:2d}: {d:6.2f}')
    print(f'{anim_name}: 平均 {tot / n:.2f}（{n} 帧）')

if __name__ == '__main__':
    main()
