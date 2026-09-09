# -*- coding: utf-8 -*-
"""打包原始 Spine 三元组（.skel/.atlas/.png）成 data_spine.js，供纯 Spine 仿真器在 file:// 下加载。
用法: python pack_spine.py <spine_raw根目录> <输出js路径> [角色名...]
"""
import base64
import json
import os
import sys


def main():
    raw_root, out_js = sys.argv[1], sys.argv[2]
    chars = {}
    for cname in sorted(os.listdir(raw_root)):
        cdir = os.path.join(raw_root, cname)
        if not os.path.isdir(cdir):
            continue
        forms = {}
        for fname in sorted(os.listdir(cdir)):
            fdir = os.path.join(cdir, fname)
            if not os.path.isdir(fdir):
                continue
            files = {}
            stem = None
            for f in sorted(os.listdir(fdir)):
                if f.endswith('.skel'):
                    stem = f[:-5]
                elif f.endswith('.json'):
                    stem = stem or f[:-5]
            if not stem:
                continue
            for ext, key in (('.skel', 'skel'), ('.json', 'skel'), ('.atlas', 'atlas')):
                p = os.path.join(fdir, stem + ext)
                if os.path.exists(p):
                    with open(p, 'rb') as fp:
                        files[key] = base64.b64encode(fp.read()).decode('ascii')
            for img in sorted(os.listdir(fdir)):
                if img.endswith('.png'):
                    with open(os.path.join(fdir, img), 'rb') as fp:
                        files['pngs'] = files.get('pngs', {})
                        files['pngs'][img] = base64.b64encode(fp.read()).decode('ascii')
            forms[fname] = files
        if forms:
            chars[cname] = forms
    with open(out_js, 'w', encoding='utf-8') as fp:
        fp.write('// 自动生成：python tools/pack_spine.py 的输出，勿手改。\n')
        fp.write('window.SPINE_DATA = %s;\n' % json.dumps(chars))
    print(f'{len(chars)} 个角色 -> {out_js} ({os.path.getsize(out_js) / 1024 / 1024:.1f} MB)')


if __name__ == '__main__':
    main()
