"""WAV → 16kHz mono converter. Place in a voice/music folder and run. Skips already-16kHz files."""
import os, subprocess, sys, json

DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(DIR)

# Check ffmpeg
try:
    subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
except Exception:
    print("ffmpeg not found. Install: https://ffmpeg.org/download.html")
    print("Done. Press Ctrl+C if needed.")
    sys.exit(1)

# Collect files
files = sorted([f for f in os.listdir(DIR) if f.lower().endswith('.wav')
                and '_16k' not in f and '_cnv' not in f])

# Pre-scan: skip already-16kHz files
to_convert = []
for f in files:
    src = os.path.join(DIR, f)
    try:
        r = subprocess.run([
            "ffprobe", "-v", "quiet", "-select_streams", "a:0",
            "-show_entries", "stream=sample_rate",
            "-of", "json", src
        ], capture_output=True, timeout=10)
        info = json.loads(r.stdout)
        rate = info["streams"][0].get("sample_rate", 0)
        if rate == 16000:
            print(f"[SKIP] {f} (already 16kHz)")
            continue
    except Exception:
        pass  # Can't probe → convert anyway
    to_convert.append(f)

print(f"Converting {len(to_convert)} .wav files to 16kHz mono...\n")

ok = bad = 0
for f in to_convert:
    src = os.path.join(DIR, f)
    tmp = os.path.join(DIR, f"{f}.tmp.wav")
    try:
        r = subprocess.run([
            "ffmpeg", "-y", "-i", src,
            "-ac", "1", "-ar", "16000", "-sample_fmt", "s16", tmp
        ], capture_output=True, timeout=60)
        if r.returncode == 0 and os.path.exists(tmp):
            os.replace(tmp, src)
            print(f"[OK] {f}")
            ok += 1
        else:
            if os.path.exists(tmp): os.remove(tmp)
            print(f"[FAIL] {f}")
            bad += 1
    except Exception as e:
        if os.path.exists(tmp): os.remove(tmp)
        print(f"[FAIL] {f}: {e}")
        bad += 1

print(f"\nDone: {ok} ok, {bad} failed, {len(files) - len(to_convert)} skipped")
print("Done. Press Ctrl+C if needed.")
