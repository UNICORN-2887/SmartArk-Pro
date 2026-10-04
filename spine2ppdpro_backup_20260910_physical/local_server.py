#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""spine2ppdpro 本地服务。

功能：
  - 静态服务当前目录的仿真器网页
  - /characters/ 挂载 tools/ppd_out
  - /api/import 从 PRTS/torappu 下载 Spine 三元组并本地转换为 PPD
"""
import argparse
import json
import os
import subprocess
import sys
import traceback
import urllib.parse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(ROOT, 'tools')
DATA_ROOT = os.path.join(ROOT, 'data')
RAW_ROOT = os.path.join(DATA_ROOT, 'spine_raw')
PPD_ROOT = os.path.join(DATA_ROOT, 'ppd_out')
GROUPS = ('正面', '背面', '基建')

sys.path.insert(0, TOOLS)
from wiki_fetch import search_operators, get_meta, download_set  # noqa: E402


def ensure_inside(root, path):
    root_abs = os.path.abspath(root)
    path_abs = os.path.abspath(path)
    if os.path.commonpath([root_abs, path_abs]) != root_abs:
        raise ValueError(f'路径越界: {path_abs}')
    return path_abs


def clean_dir(root, name):
    path = ensure_inside(root, os.path.join(root, name))
    os.makedirs(path, exist_ok=True)
    return path


def extract_query(value):
    q = (value or '').strip()
    if not q:
        return ''
    try:
        u = urllib.parse.urlparse(q)
        if u.scheme and u.netloc:
            parts = [p for p in u.path.split('/') if p]
            if len(parts) >= 2 and parts[0] == 'w':
                return urllib.parse.unquote(parts[1])
            if parts:
                return urllib.parse.unquote(parts[-1])
    except Exception:
        pass
    return q


def find_candidate(query):
    cands = search_operators(query, limit=1)
    if not cands:
        raise RuntimeError(f'未找到干员: {query}')
    cand = cands[0]
    meta = get_meta(cand['charid'])
    name = meta.get('name') or cand.get('name') or cand.get('page') or cand['charid']
    return cand, meta, name


def run_convert(src_dir, out_dir, scene_name):
    os.makedirs(out_dir, exist_ok=True)
    layer_cmd = ['node', os.path.join(TOOLS, 'render_layers.mjs'), src_dir, out_dir, scene_name, '--landscape']
    anim_cmd = ['node', os.path.join(TOOLS, 'export_anims_ppd.mjs'), src_dir, out_dir, '20']
    logs = []
    for cmd in (layer_cmd, anim_cmd):
        p = subprocess.run(cmd, cwd=TOOLS, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        logs.append(p.stdout)
        if p.returncode != 0:
            raise RuntimeError(p.stdout.strip() or f'{cmd[1]} 失败')
    return ''.join(logs)


def import_character(value):
    query = extract_query(value)
    cand, meta, name = find_candidate(query)
    disk_key = cand['charid']

    raw_char = clean_dir(RAW_ROOT, disk_key)
    ppd_char = clean_dir(PPD_ROOT, disk_key)

    downloaded = {}
    for group in GROUPS:
        dest = os.path.join(raw_char, group)
        os.makedirs(dest, exist_ok=True)
        stem = download_set(meta, group, dest)
        if stem:
            downloaded[group] = {'dir': dest, 'stem': stem}

    meta_path = os.path.join(raw_char, 'meta.json')
    with open(meta_path, 'w', encoding='utf-8') as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)

    if '正面' not in downloaded:
        raise RuntimeError(f'{name} 没有正面 Spine 数据')

    convert_logs = {}
    convert_logs['正面'] = run_convert(downloaded['正面']['dir'], ppd_char, f'{name}_正面')

    forms = []
    for group in ('背面', '基建'):
        item = downloaded.get(group)
        if not item:
            continue
        rel = f'forms/{group}'
        out_dir = os.path.join(ppd_char, 'forms', group)
        convert_logs[group] = run_convert(item['dir'], out_dir, f'{name}_{group}')
        forms.append({'name': group, 'dir': rel})

    if forms:
        with open(os.path.join(ppd_char, 'forms.json'), 'w', encoding='utf-8') as f:
            json.dump({'forms': forms}, f, ensure_ascii=False)
    with open(os.path.join(ppd_char, 'spine2ppdpro.json'), 'w', encoding='utf-8') as f:
        json.dump({'name': name, 'charid': cand['charid'], 'page': cand.get('page')}, f, ensure_ascii=False)

    return {
        'ok': True,
        'name': name,
        'charid': cand['charid'],
        'page': cand.get('page'),
        'forms': list(downloaded.keys()),
        'url': f'/characters/{urllib.parse.quote(disk_key)}/',
        'raw_dir': raw_char,
        'ppd_dir': ppd_char,
        'logs': convert_logs,
    }


def list_characters():
    os.makedirs(PPD_ROOT, exist_ok=True)
    out = []
    for disk_key in sorted(os.listdir(PPD_ROOT)):
        d = os.path.join(PPD_ROOT, disk_key)
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, 'scene.json')):
            name = disk_key
            meta_path = os.path.join(d, 'spine2ppdpro.json')
            if os.path.isfile(meta_path):
                try:
                    with open(meta_path, 'r', encoding='utf-8') as f:
                        meta = json.load(f)
                    name = meta.get('name') or disk_key
                except Exception:
                    pass
            out.append({'name': name, 'dir': disk_key, 'url': f'/characters/{urllib.parse.quote(disk_key)}/'})
    return out


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def translate_path(self, path):
        p = urllib.parse.urlsplit(path).path
        if p.startswith('/characters/'):
            rel = urllib.parse.unquote(p[len('/characters/'):])
            return ensure_inside(PPD_ROOT, os.path.join(PPD_ROOT, rel))
        return os.path.join(ROOT, urllib.parse.unquote(p.lstrip('/')))

    def send_json(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == '/api/characters':
            self.send_json(200, {'ok': True, 'characters': list_characters()})
            return
        if parsed.path == '/api/search':
            qs = urllib.parse.parse_qs(parsed.query)
            query = extract_query(qs.get('q', [''])[0])
            try:
                cands = search_operators(query, limit=6)
                for cand in cands:
                    try:
                        cand['name'] = get_meta(cand['charid']).get('name') or cand.get('page') or cand['charid']
                    except Exception:
                        cand['name'] = cand.get('page') or cand['charid']
                self.send_json(200, {'ok': True, 'query': query, 'candidates': cands})
            except Exception as e:
                self.send_json(500, {'ok': False, 'error': str(e)})
            return
        super().do_GET()

    def do_POST(self):
        if urllib.parse.urlparse(self.path).path != '/api/import':
            self.send_error(404)
            return
        try:
            length = int(self.headers.get('Content-Length', '0'))
            body = self.rfile.read(length).decode('utf-8')
            data = json.loads(body or '{}')
            value = data.get('query') or data.get('url') or data.get('charid') or ''
            result = import_character(value)
            self.send_json(200, result)
        except Exception as e:
            self.send_json(500, {
                'ok': False,
                'error': str(e),
                'trace': traceback.format_exc(limit=4),
            })

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description='spine2ppdpro 本地导入与仿真服务')
    ap.add_argument('--port', type=int, default=8088)
    ap.add_argument('--import-char', help='前台导入 PRTS 页面、角色名或 charid 后退出')
    args = ap.parse_args()
    os.makedirs(RAW_ROOT, exist_ok=True)
    os.makedirs(PPD_ROOT, exist_ok=True)
    if args.import_char:
        result = import_character(args.import_char)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    print(f'spine2ppdpro: http://127.0.0.1:{args.port}/')
    print(f'PPD 数据目录: {PPD_ROOT}')
    ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
