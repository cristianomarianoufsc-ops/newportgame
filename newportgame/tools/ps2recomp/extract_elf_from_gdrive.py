#!/usr/bin/env python3
"""
extract_elf_from_gdrive.py
Extrai o ELF de um PS2 ISO diretamente do Google Drive usando Range requests.
Só baixa os setores necessários (PVD + root dir + ELF) — muito menor que 8.5GB.
"""
import urllib.request, struct, sys, os, time, re

URL = ('https://drive.usercontent.google.com/download'
       '?id=1ruRDjG5J0FrCVSU1WdNQqehIoT7csS0S&export=download&confirm=t')
SECTOR = 2048
HEADERS = {'User-Agent': 'Mozilla/5.0'}

def fetch_range(start_byte, length, retries=5):
    for attempt in range(retries):
        try:
            req = urllib.request.Request(
                URL,
                headers={**HEADERS, 'Range': f'bytes={start_byte}-{start_byte+length-1}'}
            )
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except Exception as e:
            print(f"  [retry {attempt+1}/{retries}] {e}", flush=True)
            time.sleep(2 ** attempt)
    raise RuntimeError(f"Falhou após {retries} tentativas ao ler offset {start_byte}")

def fetch_sector(lba, count=1):
    return fetch_range(lba * SECTOR, count * SECTOR)

def parse_pvd(data):
    assert data[0] == 0x01 and data[1:6] == b'CD001'
    root = data[156:190]
    root_lba  = struct.unpack_from('<I', root, 2)[0]
    root_size = struct.unpack_from('<I', root, 10)[0]
    return root_lba, root_size

def parse_directory(data):
    entries, i = [], 0
    while i < len(data):
        rec_len = data[i]
        if rec_len == 0:
            i = ((i // SECTOR) + 1) * SECTOR; continue
        if i + rec_len > len(data): break
        lba   = struct.unpack_from('<I', data, i + 2)[0]
        size  = struct.unpack_from('<I', data, i + 10)[0]
        flags = data[i + 25]
        nlen  = data[i + 32]
        name  = data[i+33:i+33+nlen].split(b';')[0].decode('ascii', errors='replace')
        entries.append((name, lba, size, bool(flags & 0x02)))
        i += rec_len
    return entries

def find_elf(entries):
    for name, lba, size, is_dir in entries:
        if not is_dir and re.match(r'S[A-Z]{3}_\d+', name):
            return name, lba, size
    for name, lba, size, is_dir in entries:
        if not is_dir and name.upper().endswith('.ELF'):
            return name, lba, size
    return None, None, None

def main():
    dest_dir = os.path.join(os.path.dirname(__file__), 'build', 'elf_out')
    os.makedirs(dest_dir, exist_ok=True)

    print("1. Lendo PVD (setor 16)...", flush=True)
    root_lba, root_size = parse_pvd(fetch_sector(16))
    root_sectors = (root_size + SECTOR - 1) // SECTOR
    print(f"   Root dir: LBA={root_lba}, size={root_size}", flush=True)

    print("2. Lendo diretório raiz...", flush=True)
    entries = parse_directory(fetch_sector(root_lba, root_sectors))
    for name, lba, size, is_dir in entries:
        print(f"     {'[DIR]' if is_dir else f'[{size//1024}KB]':10} {name}", flush=True)

    elf_name, elf_lba, elf_size = find_elf(entries)
    if not elf_name:
        print("ERRO: ELF não encontrado!"); sys.exit(1)

    dest_name = elf_name if '.' in elf_name else elf_name + '.elf'
    dest_path = os.path.join(dest_dir, dest_name)
    print(f"3. Baixando ELF '{elf_name}' ({elf_size//1024}KB)...", flush=True)

    data, elf_sectors = bytearray(), (elf_size + SECTOR - 1) // SECTOR
    for off in range(0, elf_sectors, 16):
        chunk = min(16, elf_sectors - off)
        data.extend(fetch_sector(elf_lba + off, chunk))
        print(f"\r   {100*len(data)/elf_size:5.1f}%  {len(data)//1024}KB/{elf_size//1024}KB  ", end='', flush=True)

    with open(dest_path, 'wb') as f:
        f.write(data[:elf_size])
    print(f"\n=== ELF extraído: {dest_path} ({os.path.getsize(dest_path)} bytes) ===", flush=True)

if __name__ == '__main__':
    main()
