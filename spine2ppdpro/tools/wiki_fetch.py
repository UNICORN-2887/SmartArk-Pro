# -*- coding: utf-8 -*-
"""PRTS wiki 干员 Spine 资源爬取（数据源已逆向验证，2026-08）。

链路：
  1. 角色名搜索  https://m.prts.wiki/api.php?action=opensearch&search=<名>&format=json
                 （主站 prts.wiki 有 Tengine 反爬 curl/Python 会被 403，m 子域放行）
  2. 干员id      https://m.prts.wiki/index.php?title=<页面名>&action=raw → |干员id=char_XXX
  3. 资源清单    https://torappu.prts.wiki/assets/char_spine/<charid>/meta.json
                 → skin["默认"]["正面"/"背面"/"基建"].file + prefix
  4. 资源文件    <prefix><file>.skel/.atlas/.png（torappu 为阿里云 OSS，无风控）
"""
import json
import os
import re
import time
import urllib.parse
import urllib.request

UA = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/126.0 Safari/537.36')
OPENSEARCH = 'https://m.prts.wiki/api.php?action=opensearch&search={q}&format=json&limit=10'
RAW = 'https://m.prts.wiki/index.php?title={title}&action=raw'
META = 'https://torappu.prts.wiki/assets/char_spine/{charid}/meta.json'
CHARID_RE = re.compile(r'^char_\d+_\w+$')
ID_FIELD_RE = re.compile(r'\|\s*干员id\s*=\s*(char_\d+_\w+)')
# opensearch 会返回大量子页面，都不是干员主页面
SKIP_SUB = ('/语音记录', '/干员密录', '/spine', '/资料', '/家具', '(敌方)', '(装置)', '(召唤物)')


def _get(url, timeout=30, retries=2):
    """GET 并返回文本；429/5xx 时重试（m 子域有轻量风控，间隔拉大）"""
    last = None
    for i in range(retries + 1):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': UA})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            last = e
            if e.code in (403, 429) and i < retries:
                time.sleep(2 + i * 2)
                continue
            raise
        except Exception as e:
            last = e
            if i < retries:
                time.sleep(1)
                continue
            raise
    raise last


def search_operators(query, limit=6):
    """角色名 → [{'page': 页面名, 'charid': 干员id}, ...]。
    直接输入干员id（char_xxx_yyy）也能识别。只保留解析出干员id 的候选。"""
    q = (query or '').strip()
    if not q:
        return []
    # 直接给了干员id：验证 meta.json 存在即确认
    if CHARID_RE.match(q):
        try:
            meta = json.loads(_get(META.format(charid=q)))
            return [{'page': None, 'charid': q, 'name': meta.get('name', q)}]
        except Exception:
            return []
    data = json.loads(_get(OPENSEARCH.format(q=urllib.parse.quote(q))))
    titles = data[1] if len(data) > 1 else []
    out = []
    for t in titles:
        if any(s in t for s in SKIP_SUB):
            continue
        try:
            wikitext = _get(RAW.format(title=urllib.parse.quote(t))).decode('utf-8')
        except Exception:
            continue
        m = ID_FIELD_RE.search(wikitext)
        if not m:
            continue
        out.append({'page': t, 'charid': m.group(1)})
        time.sleep(0.4)   # m 子域防风控
        if len(out) >= limit:
            break
    return out


def get_meta(charid):
    """torappu meta.json → {prefix, name, skin:{<皮肤>:{<组>: {file}}}}"""
    return json.loads(_get(META.format(charid=charid)))


def download_set(meta, group_key, dest_dir):
    """下载 meta 中 skin['默认'][group_key] 的骨骼/图集/图集图三文件到 dest_dir。
    骨骼优先 .skel，404 时回退 .json（Spine JSON 导出）。返回文件 stem，无该组返回 None。"""
    skin = meta.get('skin', {}).get('默认', {})
    g = skin.get(group_key)
    if not g:
        return None
    prefix = meta.get('prefix', '')
    file = g['file']
    base = prefix + file
    stem = file.rsplit('/', 1)[-1]
    # 骨骼：.skel → .json 回退
    for ext in ('.skel', '.json'):
        url = base + ext
        try:
            data = _get(url, timeout=120)
        except urllib.error.HTTPError as e:
            if e.code == 404 and ext == '.skel':
                continue
            raise
        with open(os.path.join(dest_dir, stem + ext), 'wb') as f:
            f.write(data)
        break
    for ext in ('.atlas', '.png'):
        data = _get(base + ext, timeout=300)
        with open(os.path.join(dest_dir, stem + ext), 'wb') as f:
            f.write(data)
    return stem
