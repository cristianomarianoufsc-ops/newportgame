#!/usr/bin/env python3
"""download_iso.py — Baixa a ISO do God of War (USA) via Google Drive."""
import urllib.request, os, sys, time

URL = 'https://drive.usercontent.google.com/download?id=1ruRDjG5J0FrCVSU1WdNQqehIoT7csS0S&export=download&confirm=t'
DEST = os.path.join(os.path.dirname(__file__), 'build', 'God of War (USA).iso')
DEST_TMP = DEST + '.part'

def human(n):
    for unit in ['B','KB','MB','GB']:
        if n < 1024: return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"

if os.path.exists(DEST):
    print(f"ISO já existe: {human(os.path.getsize(DEST))}")
    sys.exit(0)

print(f"Baixando para {DEST} ...", flush=True)
t0 = time.time()
downloaded = 0

try:
    req = urllib.request.Request(URL, headers={'User-Agent': 'Mozilla/5.0'})
    with urllib.request.urlopen(req, timeout=30) as r, open(DEST_TMP, 'wb') as f:
        total = int(r.headers.get('Content-Length', 0))
        chunk = 1 << 20  # 1MB
        while True:
            buf = r.read(chunk)
            if not buf: break
            f.write(buf)
            downloaded += len(buf)
            elapsed = time.time() - t0
            speed = downloaded / elapsed if elapsed > 0 else 0
            eta = (total - downloaded) / speed if speed > 0 else 0
            pct = 100 * downloaded / total if total else 0
            print(f"\r  {pct:5.1f}%  {human(downloaded)}/{human(total)}  "
                  f"{human(speed)}/s  ETA {eta:.0f}s   ", end='', flush=True)
    os.rename(DEST_TMP, DEST)
    print(f"\nDownload completo: {human(os.path.getsize(DEST))}")
except Exception as e:
    print(f"\nErro: {e}")
    sys.exit(1)
