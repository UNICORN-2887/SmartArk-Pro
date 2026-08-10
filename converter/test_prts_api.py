"""Test PRTS wiki API access. Run on YOUR machine (not mine — I can't reach prts.wiki).
Outputs: page text and audio URLs found."""
import json, re, sys
try:
    import requests
except ImportError:
    print("Need requests: pip install requests")
    sys.exit(1)

# ── Config ──
PAGE = "魔王/语音记录"          # ← change for other characters
API  = "https://prts.wiki/api.php"

print(f"Testing PRTS API for page: {PAGE}\n")

# 1) Get page text via parse API
params = {
    "action": "parse",
    "page": PAGE,
    "prop": "text",
    "format": "json",
    "formatversion": 2,
}
try:
    r = requests.get(API, params=params, timeout=30)
    data = r.json()
    html = data["parse"]["text"]
    print(f"[OK] Got HTML: {len(html)} chars\n")
except Exception as e:
    print(f"[FAIL] API error: {e}")
    print(f"Response code: {r.status_code if 'r' in dir() else 'N/A'}")
    sys.exit(1)

# 2) Find audio URLs
audio_urls = re.findall(r'https?://[^"\s]+\.(?:wav|mp3|ogg)', html)
print(f"Audio files found: {len(audio_urls)}")
for u in audio_urls[:5]:
    print(f"  {u}")
if len(audio_urls) > 5:
    print(f"  ... and {len(audio_urls)-5} more")

# 3) Find voice table rows
# Each row typically has: voice line name, text, audio link
# Look for common PRTS HTML patterns
rows = re.findall(r'<tr[^>]*class="[^"]*voice[^"]*"[^>]*>.*?</tr>', html, re.DOTALL)
if not rows:
    rows = re.findall(r'<tr[^>]*>.*?</tr>', html, re.DOTALL)

# Try to find voice text within table cells
voice_items = re.findall(r'<td[^>]*>(.*?)</td>', html, re.DOTALL)
print(f"\nTable cells found: {len(voice_items)}")

# Show first few cells (stripped of HTML tags)
if voice_items:
    print("\nFirst 10 non-empty cells (stripped):")
    count = 0
    for td in voice_items:
        clean = re.sub(r'<[^>]+>', '', td).strip()
        if clean and len(clean) > 2:
            print(f"  [{count}] {clean[:120]}")
            count += 1
            if count >= 10: break

# 4) Save raw HTML for debugging
with open("prts_debug.html", "w", encoding="utf-8") as f:
    f.write(html)
print(f"\n[DONE] Full HTML saved to prts_debug.html ({len(html)} chars)")
print("Open it and check if voice lines + download links are present.")
