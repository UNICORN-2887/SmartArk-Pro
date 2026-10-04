#!/usr/bin/env python3
"""Spine → 纸偶(PPD) 一键转换：render_layers.mjs 逐层导出 + preview.py 合成预览
用法：python convert.py <spine_dir> <out_dir> [scene_name] [--landscape]
示例（横屏 Q 版互动）：python convert.py "E:/Passport/source/立绘/产出/Qlive2d/Amiya/阿米娅正面" \
       "E:/虚拟SD卡/main/operator/CASTER/5STAR/Amiya/PPD_Q" Amiya_Q --landscape
"""
import os
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    spine_dir, out_dir = sys.argv[1], sys.argv[2]
    land = '--landscape' in sys.argv
    name = next((a for a in sys.argv[3:] if not a.startswith('--')),
                os.path.basename(out_dir.rstrip('/\\')))
    here = os.path.dirname(os.path.abspath(__file__))

    cmd = ['node', os.path.join(here, 'render_layers.mjs'), spine_dir, out_dir, name]
    if land:
        cmd.append('--landscape')
    subprocess.run(cmd, check=True)

    preview = os.path.join(out_dir, 'preview.png')
    pv = [sys.executable, os.path.join(here, 'preview.py'), out_dir, preview]
    if land:
        pv.append('--landscape')
    subprocess.run(pv, check=True)
    print(f'完成 → {out_dir}（scene.json + *.raw + preview.png）')


if __name__ == '__main__':
    main()
