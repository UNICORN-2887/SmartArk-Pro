# -*- coding: utf-8 -*-
"""把背面/基建等外部形态目录注册进根 forms.json（动画子形态由 convert.mjs 写入）。
用法: python finish_forms.py <根产物目录> 背面 基建
"""
import json
import sys

root, *names = sys.argv[1:]
p = root.rstrip('/\\') + '/forms.json'
try:
    forms = json.load(open(p, encoding='utf-8'))
except FileNotFoundError:
    forms = {'forms': []}
existing = {f['dir'] for f in forms['forms']}
for n in names:
    d = f'forms/{n}'
    if d not in existing:
        forms['forms'].append({'name': n, 'dir': d})
json.dump(forms, open(p, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print('forms.json:', forms['forms'])
