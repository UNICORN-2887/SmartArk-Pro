"""拼豆拆分 Web 工具 — 本地 Flask 服务（127.0.0.1）。

用法: python app.py  →  浏览器打开 http://127.0.0.1:5000
"""

import io
import json
import re
import uuid
import zipfile
import webbrowser

import numpy as np
from flask import Flask, jsonify, request, send_file
from PIL import Image, ImageOps

import palette as palette_mod
from beads_convert import (BEADS_L1, BEADS_L2, BEADS_L3,
                           average_pool, quantize, build_export)
from beads_render import (render_tile_png, render_overview_png,
                          render_b2_png, render_palette_png)

app = Flask(__name__, static_folder='static', static_url_path='/static')

JOBS = {}      # job_id → JobResult（dict 保序，超限淘汰最旧）
MAX_JOBS = 32
MAX_DIM = 8192  # 超大图先等比缩小（裁剪精度略有损失，README 注明）

ALLOWED_N = {BEADS_L1, BEADS_L2, BEADS_L3}
NAME_RE = re.compile(r'^[A-Za-z0-9 _-]{1,20}$')  # 设备 26 键键盘只出 ASCII
OVERVIEW_CELL = {BEADS_L1: 8, BEADS_L2: 6, BEADS_L3: 3}


def _png_bytes(img):
    buf = io.BytesIO()
    img.save(buf, format='PNG')
    return buf.getvalue()


def _evict_jobs():
    while len(JOBS) >= MAX_JOBS:
        JOBS.pop(next(iter(JOBS)))


# ── 页面 ──

@app.get('/')
def index():
    return send_file('static/index.html')


@app.get('/api/palette')
def api_palette():
    """36 色 JSON：前端渲染色卡用，单一数据源。"""
    colors = [dict(id=i + 1, rgb=list(palette_mod.PALETTE_RGB[i]),
                   row=palette_mod.palette_coord(i)[0],
                   col=palette_mod.palette_coord(i)[1])
              for i in range(palette_mod.BEADS_COLORS)]
    return jsonify(colors=colors)


# ── 转换 ──

@app.post('/api/convert')
def api_convert():
    """multipart: image 文件 + n + name + crop(JSON 字符串, 可选)。

    全管线同步执行（216 档约 2~4 秒），结果存内存作业，返回 job 元数据。
    """
    f = request.files.get('image')
    if f is None:
        return jsonify(error='缺少图片文件'), 400

    try:
        n = int(request.form.get('n', ''))
    except ValueError:
        n = 0
    if n not in ALLOWED_N:
        return jsonify(error=f'n 必须是 {sorted(ALLOWED_N)} 之一'), 400

    name = (request.form.get('name') or '').strip()
    if not NAME_RE.match(name):
        return jsonify(error='名字仅限 ASCII 字母数字/空格/_/-，长度 1~20'), 400

    crop = None
    crop_raw = request.form.get('crop')
    if crop_raw:
        try:
            crop = json.loads(crop_raw)
            crop = dict(x=int(crop['x']), y=int(crop['y']), size=int(crop['size']))
        except (ValueError, KeyError, TypeError):
            return jsonify(error='crop 格式错误'), 400

    try:
        job = _convert_job(f.read(), n, name, crop)
    except Exception as e:  # PIL 解码失败等
        return jsonify(error=f'图片处理失败: {e}'), 400

    job_id = uuid.uuid4().hex
    _evict_jobs()
    JOBS[job_id] = job
    return jsonify(job_id=job_id, n=n, name=name,
                   tiles=[dict(name=t[1], pos216=t[2], pos72=t[3])
                          for t in job['tiles']],
                   b2s=[dict(name=b[1], pos216=b[2]) for b in job['b2s']])


def _convert_job(image_bytes, n, name, crop):
    img = Image.open(io.BytesIO(image_bytes))
    img = ImageOps.exif_transpose(img)
    img = img.convert('RGB')  # 透明底变黑，前端提示用白底图
    w, h = img.size

    if max(w, h) > MAX_DIM:
        scale = MAX_DIM / max(w, h)
        img = img.resize((max(1, round(w * scale)), max(1, round(h * scale))),
                         Image.LANCZOS)
        w, h = img.size
        if crop:
            crop = {k: max(0, min(img.size[0], round(v * scale))) for k, v in crop.items()}

    if crop:
        x, y, s = crop['x'], crop['y'], crop['size']
        s = max(1, min(s, w, h))
        x = max(0, min(w - s, x))
        y = max(0, min(h - s, y))
        img = img.crop((x, y, x + s, y + s))
    else:  # 无 crop：中心正方形（1:1 或前端未选框）
        s = min(w, h)
        x, y = (w - s) // 2, (h - s) // 2
        img = img.crop((x, y, x + s, y + s))

    art = quantize(average_pool(np.asarray(img, dtype=np.float32), n))
    export = build_export(art, n, name)

    overview = _png_bytes(render_overview_png(art, n, OVERVIEW_CELL[n]))
    tiles = [(_png_bytes(render_tile_png(t['art'], t['pos216'], t['pos72'])),
              f"{t['basename']}.png", t['pos216'], t['pos72'])
             for t in export['tiles']]
    b2s = [(_png_bytes(render_b2_png(b['art'], b['pos216'])),
            f"{b['basename']}.png", b['pos216'])
           for b in export['b2s']]
    palette_png = _png_bytes(render_palette_png())

    return dict(n=n, name=name, overview=overview,
                tiles=tiles, b2s=b2s, palette=palette_png, export=export)


# ── 结果图片与下载 ──

def _get_job(job_id):
    return JOBS.get(job_id)


@app.get('/api/job/<job_id>/overview.png')
def job_overview(job_id):
    job = _get_job(job_id)
    if not job:
        return 'job not found', 404
    return send_file(io.BytesIO(job['overview']), mimetype='image/png')


@app.get('/api/job/<job_id>/tiles/<fname>')
def job_tile(job_id, fname):
    job = _get_job(job_id)
    if not job:
        return 'job not found', 404
    for data, name, _, _ in job['tiles']:
        if name == fname:
            return send_file(io.BytesIO(data), mimetype='image/png')
    for data, name, _ in job['b2s']:
        if name == fname:
            return send_file(io.BytesIO(data), mimetype='image/png')
    return 'tile not found', 404


@app.get('/api/job/<job_id>/palette.png')
def job_palette(job_id):
    job = _get_job(job_id)
    if not job:
        return 'job not found', 404
    return send_file(io.BytesIO(job['palette']), mimetype='image/png')


def _readme_text(job):
    n, name = job['n'], job['name']
    return f"""拼豆成品包  {name}  (N={n})
====================================

一、懒人导入（设备端成品数据）
把本 zip 里 beads/ 目录下的所有 .bead 文件拷到 SD 卡的
/sdcard/User/Beads/ 目录（没有则让设备端保存一次自动创建），
设备端「一起拼豆」界面即可读取：
- B3_*.bead : 216x216 整幅（三级合成界面按「加载」直接打开）
- B2_*.bead : 72x72 块（二级合成界面按「加载」打开，或三级合成板选块放入）
- B1_*.bead : 24x24 最小块（一级编辑器「加载」打开，或二级合成板选块放入）

二、对照拼图
images/ 下的 PNG 与 .bead 同名对应：
- overview_{n}.png : 整幅总览
- B1_*.png         : 24x24 块对照图，每格标有设备 6x6 色卡坐标（行-列）
- B2_*.png         : 72x72 块辅助图
- palette_36.png   : 36 色卡说明（色号 + 色卡坐标）

三、注意
- 文件名仅 ASCII（设备端键盘限制）
- 透明底图片按黑底处理，建议用白底图
"""


@app.get('/api/job/<job_id>/download')
def job_download(job_id):
    job = _get_job(job_id)
    if not job:
        return 'job not found', 404
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for fname, data in job['export']['beads']:
            z.writestr(f'beads/{fname}', data)
        z.writestr(f'images/overview_{job["n"]}.png', job['overview'])
        for data, fname, _, _ in job['tiles']:
            z.writestr(f'images/{fname}', data)
        for data, fname, _ in job['b2s']:
            z.writestr(f'images/{fname}', data)
        z.writestr('palette_36.png', job['palette'])
        z.writestr('README.txt', _readme_text(job))
    buf.seek(0)
    return send_file(buf, mimetype='application/zip', as_attachment=True,
                     download_name=f"beads_{job['name']}_{job['n']}.zip")


if __name__ == '__main__':
    url = 'http://127.0.0.1:5000'
    print(f'拼豆拆分工具已启动: {url}  (Ctrl+C 退出)')
    webbrowser.open(url)
    app.run(host='127.0.0.1', port=5000, debug=False)
