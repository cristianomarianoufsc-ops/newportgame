#!/usr/bin/env python3
"""
download_iso_robust.py
Download resumível da ISO do God of War via Google Drive.
Usa Range requests para retomar de onde parou, com retry automático.
Grava progresso a cada 32MB para sobreviver a kills.
"""
import urllib.request, os, sys, time, signal, struct

URL = ('https://drive.usercontent.google.com/download'
       '?id=1ruRDjG5J0FrCVSU1WdNQqehIoT7csS0S&export=download&confirm=t')
HEADERS = {'User-Agent': 'Mozilla/5.0'}

DEST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build')
DEST     = os.path.join(DEST_DIR, 'God of War (USA).iso')
DEST_TMP = DEST + '.part'
CHUNK    = 4 * 1024 * 1024   # 4MB por chunk
FLUSH_EVERY = 32 * 1024 * 1024  # flush / print a cada 32MB

def human(n):
    for u in ['B','KB','MB','GB']:
        if abs(n) < 1024: return f"{n:.1f}{u}"
        n /= 1024
    return f"{n:.1f}TB"

def get_total_size():
    req = urllib.request.Request(URL, headers={**HEADERS, 'Range': 'bytes=0-0'})
    with urllib.request.urlopen(req, timeout=30) as r:
        cr = r.headers.get('Content-Range', '')   # "bytes 0-0/8522792960"
        if '/' in cr:
            return int(cr.split('/')[-1])
        return int(r.headers.get('Content-Length', 0))

def download():
    os.makedirs(DEST_DIR, exist_ok=True)

    if os.path.exists(DEST):
        sz = os.path.getsize(DEST)
        print(f"ISO já completa: {human(sz)}")
        return

    # Tamanho já baixado
    start = os.path.getsize(DEST_TMP) if os.path.exists(DEST_TMP) else 0

    print(f"Obtendo tamanho total...", flush=True)
    total = get_total_size()
    print(f"Total: {human(total)}", flush=True)

    if start >= total:
        os.rename(DEST_TMP, DEST)
        print("Já completo — renomeado.")
        return

    print(f"Resumindo de {human(start)} / {human(total)} ({100*start/total:.1f}%)", flush=True)
    t0 = time.time()
    downloaded = start
    last_flush = downloaded

    with open(DEST_TMP, 'ab') as f:
        pos = start
        while pos < total:
            end = min(pos + CHUNK - 1, total - 1)
            for attempt in range(8):
                try:
                    req = urllib.request.Request(
                        URL,
                        headers={**HEADERS, 'Range': f'bytes={pos}-{end}'}
                    )
                    with urllib.request.urlopen(req, timeout=60) as r:
                        data = r.read()
                    if not data:
                        raise RuntimeError("chunk vazio")
                    break
                except Exception as e:
                    wait = 2 ** attempt
                    print(f"\n  [retry {attempt+1}/8 após {wait}s] {e}", flush=True)
                    time.sleep(wait)
            else:
                print("\nFALHOU — muitas tentativas. Execute novamente para retomar.")
                sys.exit(1)

            f.write(data)
            pos += len(data)
            downloaded += len(data)

            if downloaded - last_flush >= FLUSH_EVERY:
                f.flush()
                last_flush = downloaded
                elapsed = time.time() - t0
                speed = (downloaded - start) / elapsed if elapsed else 0
                eta   = (total - downloaded) / speed if speed else 0
                pct   = 100 * downloaded / total
                print(f"  {pct:5.1f}%  {human(downloaded)}/{human(total)}"
                      f"  {human(speed)}/s  ETA {eta:.0f}s", flush=True)

    os.rename(DEST_TMP, DEST)
    print(f"\nDownload completo: {human(os.path.getsize(DEST))}")

if __name__ == '__main__':
    download()
