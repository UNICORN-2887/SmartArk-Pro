# -*- coding: utf-8 -*-
"""把 PPD 产物目录打包成 data_tx.js（deflate 压缩 + base64 内嵌），供 simulator 在 file:// 下自动加载。

raw 贴图有大量透明像素，deflate 压缩率约 5~10 倍（RGBA 原始 103MB → 约 15MB）。

用法:
    python pack.py <ppd目录> <输出js路径> [内置显示名]
例:
    python pack.py ppd_out/缄默德克萨斯 ..\\data_tx.js 缄默德克萨斯
"""
import base64
import json
import os
import sys
import zlib


def main():
    ppd_dir, out_js = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(ppd_dir.rstrip('/\\'))
    files = {}
    total_raw = 0
    total_packed = 0
    for root, _dirs, fnames in os.walk(ppd_dir):
        for f in sorted(fnames):
            full = os.path.join(root, f)
            rel = os.path.relpath(full, ppd_dir).replace('\\', '/')
            with open(full, 'rb') as fp:
                data = fp.read()
            total_raw += len(data)
            packed = zlib.compress(data, 9)
            total_packed += len(packed)
            files[rel] = base64.b64encode(packed).decode('ascii')
    with open(out_js, 'w', encoding='utf-8') as fp:
        fp.write('// 自动生成：python tools/pack.py 的输出，勿手改。deflate 压缩。\n')
        fp.write('window.SPINE_PPD = %s;\n' % json.dumps({'name': name, 'files': files, 'compressed': True}))
    print(f'{name}: {len(files)} 个文件, 原始 {total_raw / 1024 / 1024:.1f} MB -> 压缩 {total_packed / 1024 / 1024:.1f} MB '
          f'-> {out_js} ({os.path.getsize(out_js) / 1024 / 1024:.1f} MB base64)')


if __name__ == '__main__':
    main()
