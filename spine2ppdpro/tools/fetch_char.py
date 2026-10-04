# -*- coding: utf-8 -*-
"""下载 PRTS 干员 Spine 三元组（复用 wiki_fetch.py 的搜索+下载链路）。

用法:
    python fetch_char.py 缄默德克萨斯
    python fetch_char.py char_1028_texas2

输出目录:
    ../data/spine_raw/<干员名>/<正面|背面|基建>/<stem>.skel|.json + .atlas + .png
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_fetch import search_operators, get_meta, download_set

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GROUPS = ('正面', '背面', '基建')


def main():
    if len(sys.argv) < 2:
        print('用法: python fetch_char.py <干员名或charid>')
        return 1
    query = sys.argv[1]
    cands = search_operators(query, limit=1)
    if not cands:
        print(f'未找到干员: {query}')
        return 1
    cand = cands[0]
    charid = cand['charid']
    name = cand.get('name') or (cand.get('page') or charid)
    print(f'命中: {name} ({charid})')

    meta = get_meta(charid)
    out_root = os.path.join(ROOT, 'data', 'spine_raw', name)
    for g in GROUPS:
        d = os.path.join(out_root, g)
        os.makedirs(d, exist_ok=True)
        stem = download_set(meta, g, d)
        if stem:
            print(f'  [{g}] -> {os.path.join(d, stem)}')
        else:
            print(f'  [{g}] -> （无该形态）')
    # 保存 meta 供转换时参考（皮肤名等）
    with open(os.path.join(out_root, 'meta.json'), 'w', encoding='utf-8') as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
    print(f'完成: {out_root}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
