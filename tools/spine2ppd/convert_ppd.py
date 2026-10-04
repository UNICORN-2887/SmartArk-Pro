#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Spine→PPD 一键转换器：转换 → 自动接入 PC 仿真器预览 → 确认后导出。

用法:
  python convert_ppd.py <spine_dir> <out_dir> [scene_name]

流程:
  1. node render_layers.mjs <spine_dir> <out_dir> <name> --landscape   # 逐层场景导出
  2. node export_anims_ppd.mjs <spine_dir> <out_dir> 20                # 动作时间轴导出
  3. 把 <out_dir> 挂到仿真器 characters/<name>（junction 实时同步）+ 注册 index.json
  4. 提示打开 http://127.0.0.1:7861/?char=<name> 预览（横屏场景自动逆时针 90° 显示）

预览确认没问题后，<out_dir> 就是最终 PPD 数据（scene.json + <层>.raw + anims.json），
整个目录拷到设备 SD 卡 operator 路径即可。转换器重跑后浏览器刷新即见新数据。
"""
import os, sys, json, subprocess, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
SIM_CHARS = r'E:\Passport\source\live2d\paperdoll_sim\static\characters'   # 仿真器角色目录
SIM_URL = 'http://127.0.0.1:7861/?char={}'

def run(cmd):
    print('  >', ' '.join(cmd))
    r = subprocess.run(cmd, cwd=HERE)
    if r.returncode != 0:
        print(f'[失败] 退出码 {r.returncode}: {" ".join(cmd)}')
        sys.exit(1)

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    spine_dir = os.path.abspath(sys.argv[1])
    out_dir = os.path.abspath(sys.argv[2])
    name = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(out_dir)
    node = shutil.which('node')
    if not node:
        print('[失败] 未找到 node（spine-canvas 渲染需要）'); sys.exit(1)
    if not os.path.isdir(spine_dir):
        print(f'[失败] Spine 目录不存在: {spine_dir}'); sys.exit(1)
    os.makedirs(out_dir, exist_ok=True)
    print(f'== Spine→PPD 转换: {spine_dir} → {out_dir} ({name}) ==')

    print('[1/3] 逐层场景导出（render_layers --landscape）…')
    run([node, 'render_layers.mjs', spine_dir, out_dir, name, '--landscape'])

    print('[2/3] 动作时间轴导出（export_anims_ppd）…')
    run([node, 'export_anims_ppd.mjs', spine_dir, out_dir, '20'])

    print('[3/3] 接入 PC 仿真器预览…')
    link = os.path.join(SIM_CHARS, name)
    if os.path.islink(link) or os.path.exists(link):
        if os.path.isdir(link) and not os.path.islink(link):
            print(f'  [警告] characters/{name} 已存在且非链接，跳过挂载（可能是真实目录）')
        else:
            print(f'  已挂载: {link}')
    else:
        r = subprocess.run(['cmd', '/c', 'mklink', '/J', link, out_dir], capture_output=True, text=True)
        if r.returncode != 0:
            print(f'  [警告] junction 失败: {r.stderr.strip()}——可手动: mklink /J "{link}" "{out_dir}"')
        else:
            print(f'  挂载成功: {link} → {out_dir}')
    idx_path = os.path.join(SIM_CHARS, 'index.json')
    try:
        names = json.load(open(idx_path, encoding='utf-8'))
        if name not in names:
            names.append(name)
            json.dump(names, open(idx_path, 'w', encoding='utf-8'), ensure_ascii=False)
            print(f'  index.json 已注册 {name}')
    except Exception as e:
        print(f'  [警告] index.json 更新失败: {e}')

    print()
    print('== 完成。预览: ' + SIM_URL.format(name))
    print('   横屏场景自动逆时针 90° 显示（头朝上）；动作下拉可播放。')
    print('   预览确认 OK 后，把该目录整个拷到 SD 卡即导出 PPD：')
    print('   ' + out_dir)
    print('   重跑转换器后浏览器刷新（或点"重载 scene.json"）即见新数据。')

if __name__ == '__main__':
    main()
