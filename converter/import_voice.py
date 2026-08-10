"""PRTS Wiki → SD卡 voice 一键导入.
Usage:
  python import_voice.py --page "魔王/语音记录" --dest "E:\SD\...\Civilight_Eterna\voice"
  python import_voice.py --page "魔王/语音记录" --dest "..." --dry-run

Pipeline: API→parse→download→rename(CN→EN)→sort(daily/fight/promo)→convert 16kHz→gen text.yaml
"""
import os, re, sys, subprocess, shutil, json, io
from urllib.parse import urljoin, unquote

try:
    import requests
except ImportError:
    print("Need requests: pip install requests")
    sys.exit(1)

# ═══════════════════════════════════════════
# 中文→英文 文件名映射（所有角色通用）
# ═══════════════════════════════════════════
CN2EN = {
    "任命助理": ("daily", "assign_assit"),
    "交谈1": ("daily", "conver1"),
    "交谈2": ("daily", "conver2"),
    "交谈3": ("daily", "conver3"),
    "晋升后交谈1": ("promotion", "pmconver1"),
    "晋升后交谈2": ("promotion", "pmconver2"),
    "信赖提升后交谈1": ("promotion", "trustpmconver1"),
    "信赖提升后交谈2": ("promotion", "trustpmconver2"),
    "信赖提升后交谈3": ("promotion", "trustpmconver3"),
    "闲置": ("daily", "idle"),
    "干员报到": ("daily", "enroll"),
    "观看作战记录": ("daily", "exp_watching"),
    "精英化晋升1": ("promotion", "elitepm1"),
    "精英化晋升2": ("promotion", "elitepm2"),
    "编入队伍": ("fight", "include"),
    "任命队长": ("fight", "assigncap"),
    "行动出发": ("fight", "misgo"),
    "行动开始": ("fight", "misstart"),
    "选中干员1": ("fight", "sel1"),
    "选中干员2": ("fight", "sel2"),
    "部署1": ("fight", "alloc1"),
    "部署2": ("fight", "alloc2"),
    "作战中1": ("fight", "combating1"),
    "作战中2": ("fight", "combating2"),
    "作战中3": ("fight", "combating3"),
    "作战中4": ("fight", "combating4"),
    "完成高难行动": ("fight", "diff"),
    "3星结束行动": ("fight", "perfect"),
    "非3星结束行动": ("fight", "inperfect"),
    "行动失败": ("fight", "misfail"),
    "进驻设施": ("daily", "assignfaculty"),
    "戳一下": ("daily", "poke"),
    "信赖触摸": ("daily", "touch"),
    "标题": ("daily", "Arknights"),
    "新年祝福": ("daily", "newyear"),
    "问候": ("daily", "greeting"),
    "生日": ("daily", "birthday"),
    "周年庆典": ("daily", "annivers"),
}

API_URL = "https://prts.wiki/api.php"
AUDIO_BASE = "https://torappu.prts.wiki/assets/audio/voice"

# ═══════════════════════════════════════════

def fetch_page(page_title):
    """Call MediaWiki parse API, return HTML string."""
    params = {"action": "parse", "page": page_title, "prop": "text", "format": "json", "formatversion": 2}
    r = requests.get(API_URL, params=params, timeout=30)
    r.raise_for_status()
    data = r.json()
    return data["parse"]["text"]

def parse_voice_data(html):
    """Extract voice items and text from PRTS page HTML. Returns list of dicts."""
    # Find voice-data-root
    root_m = re.search(r'<div\s+id="voice-data-root"([^>]*)>', html)
    if not root_m:
        raise ValueError("voice-data-root not found in HTML")

    attrs = root_m.group(1)
    voice_key = re.search(r'data-voice-key="([^"]+)"', attrs).group(1)
    voice_base_raw = re.search(r'data-voice-base="([^"]+)"', attrs).group(1)
    # voice_base_raw: "日语:voice/char_xxx,中文-普通话:voice_cn/char_xxx,..."
    cn_base = None
    for part in voice_base_raw.split(","):
        if "中文-普通话" in part:
            cn_base = part.split(":", 1)[1].strip()
            break
    if not cn_base:
        raise ValueError("No 中文-普通话 voice base found")

    # Parse voice-data-item divs
    item_divs = re.findall(r'<div\s+class="voice-data-item"([^>]*)>', html)
    items = []
    for div in item_divs:
        title = re.search(r'data-title="([^"]+)"', div)
        fn = re.search(r'data-voice-filename="([^"]+)"', div)
        if title and fn:
            items.append({
                "title": title.group(1),
                "filename": fn.group(1),
                "voice_key": voice_key,
                "cn_base": cn_base,
            })

    # Parse voice-item-detail for Chinese text
    # Each item has 5 detail divs (中文, 日文, 英文, 韩文, 中文追悼)
    # The text content is inside <span> or plain text
    detail_divs = re.findall(
        r'<div\s+class="voice-item-detail"[^>]*data-kind-name="([^"]*)"[^>]*>(.*?)</div>',
        html, re.DOTALL
    )

    # Map: index within all-details → Chinese text
    cn_texts = []
    for kind, content in detail_divs:
        if kind == "中文":
            clean = re.sub(r'<[^>]+>', '', content).strip()
            cn_texts.append(clean)

    # Pair with items (each item has one Chinese detail)
    for i, item in enumerate(items):
        if i < len(cn_texts):
            item["text"] = cn_texts[i]
        else:
            item["text"] = ""

    return items, voice_key, cn_base

def download_wav(item, dest_dir, dry_run=False):
    """Download single WAV to dest_dir/filename. Returns True on success."""
    url = f"{AUDIO_BASE}/{item['voice_key']}/{item['filename'].lower()}"
    dest = os.path.join(dest_dir, item["filename"])
    if os.path.exists(dest):
        print(f"  [SKIP] {item['filename']} (exists)")
        return True
    if dry_run:
        print(f"  [DRY] {url} → {item['filename']}")
        return True
    try:
        r = requests.get(url, timeout=60, stream=True)
        r.raise_for_status()
        with open(dest, "wb") as f:
            for chunk in r.iter_content(8192):
                f.write(chunk)
        print(f"  [OK] {item['filename']} ({os.path.getsize(dest)/1024:.0f}KB)")
        return True
    except Exception as e:
        print(f"  [FAIL] {item['filename']}: {e}")
        return False

def run_wav_converter(subdir):
    """Run wav_converter.py on a subdirectory (16kHz conversion)."""
    script = os.path.join(os.path.dirname(__file__), "wav_converter.py")
    if not os.path.exists(script):
        print(f"  [WARN] wav_converter.py not found, skipping conversion for {subdir}")
        return
    # Copy script to subdir, run, clean up
    tmp_py = os.path.join(subdir, "_wav_converter.py")
    shutil.copy2(script, tmp_py)
    try:
        result = subprocess.run([sys.executable, tmp_py], cwd=subdir,
                                capture_output=True, timeout=300)
        # Print last few lines
        for line in result.stdout.decode("utf-8", errors="replace").split("\n")[-5:]:
            if line.strip(): print(f"    {line.strip()}")
    except Exception as e:
        print(f"  [WARN] Converter failed for {subdir}: {e}")
    finally:
        os.remove(tmp_py)

def main():
    import argparse
    ap = argparse.ArgumentParser(description="PRTS Wiki → SD卡 voice 一键导入")
    ap.add_argument("--page", required=True, help="PRTS wiki page title, e.g. '魔王/语音记录'")
    ap.add_argument("--dest", required=True, help="Destination voice/ folder on SD card")
    ap.add_argument("--dry-run", action="store_true", help="Parse only, no download")
    ap.add_argument("--no-convert", action="store_true", help="Skip 16kHz conversion")
    ap.add_argument("--no-download", action="store_true", help="Skip WAV download")
    args = ap.parse_args()

    dest = args.dest
    os.makedirs(dest, exist_ok=True)

    # 1. Fetch & parse
    print(f"Fetching page: {args.page}")
    html = fetch_page(args.page)
    items, voice_key, cn_base = parse_voice_data(html)
    print(f"Parsed: {len(items)} voice items (voice_key={voice_key})\n")

    # 2. Download WAVs
    if not args.no_download:
        dl_dir = os.path.join(dest, "_download")
        os.makedirs(dl_dir, exist_ok=True)
        ok = fail = 0
        for item in items:
            if download_wav(item, dl_dir, args.dry_run): ok += 1
            else: fail += 1
        if args.dry_run:
            print(f"\nDry run done. {ok}/{len(items)} items. Exiting.")
            return
        print(f"\nDownloaded: {ok} ok, {fail} failed\n")
    else:
        dl_dir = dest  # use dest directly

    # 3. Rename & sort into daily/fight/promotion
    not_mapped = 0
    for sub in ("daily", "fight", "promotion"):
        os.makedirs(os.path.join(dest, sub), exist_ok=True)

    for item in items:
        cn_title = item["title"]
        mapping = CN2EN.get(cn_title)
        if not mapping:
            print(f"  [WARN] No mapping for: {cn_title}")
            not_mapped += 1
            continue
        cat, en_name = mapping
        src = os.path.join(dl_dir, item["filename"]) if not args.no_download else os.path.join(dest, item["filename"])
        dst = os.path.join(dest, cat, f"{en_name}.wav")
        if os.path.exists(src):
            if not os.path.exists(dst):
                shutil.copy2(src, dst)
                print(f"  [{cat}] {item['filename']} → {en_name}.wav")
        else:
            # Try to find by filename in dest
            alt = os.path.join(dest, item["filename"])
            if os.path.exists(alt):
                shutil.copy2(alt, dst)
                print(f"  [{cat}] {item['filename']} → {en_name}.wav (from dest)")
            else:
                print(f"  [MISS] {item['filename']} not found (skipping {cn_title})")

    # Clean up download temp
    if not args.no_download and dl_dir != dest:
        shutil.rmtree(dl_dir, ignore_errors=True)

    if not_mapped:
        print(f"\n[WARN] {not_mapped} items had no CN→EN mapping. Check CN2EN dict.")

    # 4. 16kHz conversion
    if not args.no_convert:
        print("\nConverting to 16kHz...")
        for sub in ("daily", "fight", "promotion"):
            sd = os.path.join(dest, sub)
            if os.listdir(sd):
                print(f"  {sub}/")
                run_wav_converter(sd)

    # 5. Generate text.yaml
    yaml_path = os.path.join(dest, "text.yaml")
    with open(yaml_path, "w", encoding="utf-8") as f:
        for item in items:
            mapping = CN2EN.get(item["title"])
            if mapping and item.get("text"):
                en_name = mapping[1]
                # Clean text: remove parenthetical notes
                text = re.sub(r'[（(][^）)]*[）)]', '', item["text"]).strip()
                f.write(f"{en_name} : {text}\n")
    sz = os.path.getsize(yaml_path)
    print(f"\ntext.yaml: {sz} bytes ({len(items)} entries)")

    print("\n[DONE] Voice import complete!")
    print(f"  → {dest}")
    print("  Structure: daily/ fight/ promotion/ text.yaml")

if __name__ == "__main__":
    main()
