#!/usr/bin/env python3
"""spine2ppdsimulator 本地静态服务。

服务当前目录（仿真器网页本身），并可选地把角色数据根目录挂载到 /characters/：
    python serve.py                          # 只服务仿真器
    python serve.py --data D:\\spine_data    # 数据挂到 /characters/
    python serve.py --data /opt/szfz/ppd_assets/spine_imports/u3 --port 8080

打开 http://127.0.0.1:8080/ ，URL 输入框填 /characters/<角色名>/ 即可加载。
带 CORS 头与 no-store，可直接被任意静态服务器替代（只要数据同源或允许跨域）。
"""
import argparse
import http.server
import os
import urllib.parse


class Handler(http.server.SimpleHTTPRequestHandler):
    data_root = None

    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def translate_path(self, path):
        p = urllib.parse.urlsplit(path).path
        if p.startswith('/characters/') and Handler.data_root:
            rel = urllib.parse.unquote(p[len('/characters/'):])
            return os.path.join(Handler.data_root, rel)
        return super().translate_path(path)

    def log_message(self, fmt, *args):
        pass  # 静默访问日志


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='spine2ppdsimulator 本地服务')
    ap.add_argument('--data', help='角色数据根目录（挂载到 /characters/）')
    ap.add_argument('--port', type=int, default=8080)
    args = ap.parse_args()
    Handler.data_root = os.path.abspath(args.data) if args.data else None
    if Handler.data_root:
        print(f'角色数据目录: {Handler.data_root}  ->  http://127.0.0.1:{args.port}/characters/')
    print(f'仿真器: http://127.0.0.1:{args.port}/')
    http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()
