#!/usr/bin/env python3
"""
triage_headless.py — Analisa saída do modo --headless do ps2_game
==================================================================
Lê stderr capturado de:
  ps2_game --headless --frames 30 SCUS_973.99.elf 2>&1 | tee /tmp/headless.log

e entrega um relatório estruturado:

  - Resultado: PROGRESSO REAL / BLOQUEADO / CRASH
  - Frames completados e GS writes
  - Top-15 endereços quentes do PC sampler (spin loops)
  - Syscalls mais chamadas
  - Último PC registrado antes de parar
  - Diagnóstico do bloqueador (se frames = 0)

CRITÉRIO DE PROGRESSO REAL (análogo ao C838-GUARD NATIVOS do outro projeto):
  frames_completados > 0   → código PS2 nativo avançou o boot
  frames_completados = 0   → bloqueado; o hot PC é o suspeito principal

Uso:
  python3 tools/ps2recomp/triage_headless.py /tmp/headless.log
  python3 tools/ps2recomp/triage_headless.py /tmp/headless.log --short
  python3 tools/ps2recomp/triage_headless.py /tmp/headless.log --compare /tmp/headless_prev.log
"""
import re
import sys
import argparse
from collections import Counter
from pathlib import Path

SEP = "=" * 64

# ── Padrões de linha ──────────────────────────────────────────────
RE_FRAME       = re.compile(r'\[HEADLESS\] frame\s+(\d+)\s+gs_writes=(\d+)')
RE_REACHED     = re.compile(r'\[HEADLESS\] Reached (\d+) frames')
RE_STOPPED     = re.compile(r'\[HEADLESS\] ps2_game_start\(\) returned')
RE_CRASH       = re.compile(r'\[CRASH\] Signal (\d+) — (.+)')
RE_CRASH_PC    = re.compile(r'\[CRASH\] last tracked PC\s*: (0x[0-9a-fA-F]+)')
RE_CRASH_FR    = re.compile(r'\[CRASH\] frames completed\s*: (\d+)')
RE_CRASH_GS    = re.compile(r'\[CRASH\] GS register writes: (\d+)')
RE_SAMPLER     = re.compile(r'\[SAMPLER\] PC histogram.*total=(\d+) samples')
RE_SAMPLE_LINE = re.compile(r'0x([0-9a-fA-F]+)\s*:\s*(\d+)\s+\(([0-9.]+)%\)')
RE_SYSCALL     = re.compile(r'\[SYSCALL\]\s+(\S+)\s+calls=(\d+)')
RE_DISPATCH    = re.compile(r'\[DISPATCH\] UNKNOWN target (0x[0-9a-fA-F]+) \(from pc=(0x[0-9a-fA-F]+)\)')
RE_HOST_ELF    = re.compile(r'\[HOST\] Loading ELF: (.+)')
RE_HOST_ENTRY  = re.compile(r'\[HOST\] ELF entry=(0x[0-9a-fA-F]+)')
RE_SIGALRM     = re.compile(r'\[HEADLESS\] SIGALRM')
RE_FRAME_HEADL = re.compile(r'\[HEADLESS\] frame\s+(\d+)')
RE_GS_WRITES_H = re.compile(r'gs_writes=(\d+)')

def parse_log(text: str) -> dict:
    result = {
        'frames': 0,
        'gs_writes': 0,
        'crash': None,
        'crash_pc': None,
        'crash_frames': None,
        'crash_gs': None,
        'reached_target': False,
        'sigalrm': False,
        'sampler_total': 0,
        'hot_pcs': [],       # list of (pc_hex, count, pct)
        'syscalls': [],      # list of (name, count)
        'dispatches': [],    # list of (target, from_pc) — unknown JALR targets
        'elf_path': None,
        'elf_entry': None,
    }

    for line in text.splitlines():
        m = RE_FRAME.search(line)
        if m:
            result['frames'] = int(m.group(1))
            result['gs_writes'] = int(m.group(2))
            continue

        m = RE_REACHED.search(line)
        if m:
            result['reached_target'] = True
            result['frames'] = int(m.group(1))
            continue

        m = RE_CRASH.search(line)
        if m:
            result['crash'] = f"Signal {m.group(1)} — {m.group(2)}"
            continue

        m = RE_CRASH_PC.search(line)
        if m: result['crash_pc'] = m.group(1); continue

        m = RE_CRASH_FR.search(line)
        if m: result['crash_frames'] = int(m.group(1)); continue

        m = RE_CRASH_GS.search(line)
        if m: result['crash_gs'] = int(m.group(1)); continue

        m = RE_SAMPLER.search(line)
        if m: result['sampler_total'] = int(m.group(1)); continue

        m = RE_SAMPLE_LINE.search(line)
        if m:
            result['hot_pcs'].append((
                '0x' + m.group(1).upper(),
                int(m.group(2)),
                float(m.group(3))
            ))
            continue

        m = RE_SYSCALL.search(line)
        if m:
            result['syscalls'].append((m.group(1), int(m.group(2))))
            continue

        m = RE_DISPATCH.search(line)
        if m:
            result['dispatches'].append((m.group(1), m.group(2)))
            continue

        m = RE_HOST_ELF.search(line)
        if m: result['elf_path'] = m.group(1); continue

        m = RE_HOST_ENTRY.search(line)
        if m: result['elf_entry'] = m.group(1); continue

        if RE_SIGALRM.search(line):
            result['sigalrm'] = True

    # Se crash, sobrescreve frames/gs com os do crash report
    if result['crash']:
        if result['crash_frames'] is not None:
            result['frames'] = result['crash_frames']
        if result['crash_gs'] is not None:
            result['gs_writes'] = result['crash_gs']

    return result

def verdict(r: dict) -> tuple[str, str]:
    """Retorna (status, cor_emoji)."""
    if r['crash']:
        return 'CRASH', '💀'
    if r['reached_target']:
        return 'PROGRESSO REAL ✅', '🟢'
    if r['frames'] > 0:
        return 'PROGRESSO PARCIAL', '🟡'
    return 'BLOQUEADO', '🔴'

def print_report(r: dict, label: str = ""):
    status, emoji = verdict(r)
    print(f"\n{SEP}")
    print(f" {emoji}  {status}  {label}")
    print(SEP)

    print(f"  ELF carregado   : {r['elf_path'] or '(não fornecido)'}")
    if r['elf_entry']:
        print(f"  ELF entry point : {r['elf_entry']}")
    print(f"  Frames completos: {r['frames']}")
    print(f"  GS writes       : {r['gs_writes']}")
    if r['sigalrm']:
        print(f"  ⏰ SIGALRM disparou (timeout 25s sem atingir N frames)")

    if r['crash']:
        print(f"\n  ⚠️  CRASH: {r['crash']}")
        if r['crash_pc']:
            print(f"     Último PC   : {r['crash_pc']}")

    # PC sampler — hot spots (spin loops)
    if r['hot_pcs']:
        print(f"\n  PC sampler ({r['sampler_total']} amostras) — Top spin loops:")
        for pc, cnt, pct in r['hot_pcs'][:15]:
            bar = '█' * int(pct / 5)
            flag = ' ← SUSPEITO PRINCIPAL' if pct > 30 else ''
            print(f"    {pc}  {pct:5.1f}%  {bar}{flag}")

    # Syscalls
    if r['syscalls']:
        top = sorted(r['syscalls'], key=lambda x: -x[1])[:10]
        print(f"\n  Syscalls mais chamadas:")
        for name, cnt in top:
            print(f"    {name:<30} {cnt:>8,}")

    # Unknown JALR dispatches
    if r['dispatches']:
        dc = Counter(r['dispatches'])
        print(f"\n  DISPATCH desconhecidos (JALR sem alvo):")
        for (target, from_pc), cnt in dc.most_common(5):
            print(f"    target={target}  from={from_pc}  ({cnt}x)")

    # Diagnóstico
    print(f"\n  Diagnóstico:")
    if r['reached_target']:
        print(f"    ✅ {r['frames']} frames completados por código PS2 nativo.")
        print(f"    → Próximo passo: aumentar --frames ou ativar display SDL2.")
    elif r['frames'] == 0 and r['hot_pcs']:
        top_pc, top_cnt, top_pct = r['hot_pcs'][0]
        print(f"    🔴 Zero frames — spin loop em {top_pc} ({top_pct:.0f}% das amostras).")
        print(f"    → Analisar bios_stub.cpp: alguma syscall retornando sem sinalizar?")
        print(f"    → Endereço suspeito: {top_pc}")
        if r['dispatches']:
            t, fp = r['dispatches'][0]
            print(f"    → Também: JALR para {t} (de {fp}) sem handler — function gap?")
    elif r['crash']:
        print(f"    💀 Crash em {r['crash_pc'] or 'PC desconhecido'} após {r['frames']} frames.")
        print(f"    → Verificar: acesso a memória fora da RAM PS2 (0x{0x2000000:08X}+)?")
    elif r['frames'] > 0:
        print(f"    🟡 {r['frames']} frames — progresso parcial.")
        if r['hot_pcs']:
            top_pc = r['hot_pcs'][0][0]
            print(f"    → Ainda spinando em {top_pc} — implementar syscall/handler pendente.")
    else:
        print(f"    🔴 Zero frames, sem sampler data. ELF carregado?")
    print()

def compare_reports(prev: dict, curr: dict):
    """Delta entre dois runs."""
    print(f"\n  📊 Delta vs run anterior:")
    df = curr['frames'] - prev['frames']
    dg = curr['gs_writes'] - prev['gs_writes']
    sign_f = '+' if df >= 0 else ''
    sign_g = '+' if dg >= 0 else ''
    print(f"    Frames    : {prev['frames']} → {curr['frames']}  ({sign_f}{df})")
    print(f"    GS writes : {prev['gs_writes']} → {curr['gs_writes']}  ({sign_g}{dg})")
    if df > 0:
        print(f"    ✅ AVANÇO NATIVO — frames cresceram.")
    elif df == 0 and dg > 0:
        print(f"    🟡 GS writes cresceram mas frames não — GS ativado mas sem FINISH.")
    else:
        print(f"    🔴 Sem avanço de frames — round diagnóstico.")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', help='Arquivo de log do headless (stderr capturado)')
    ap.add_argument('--short', action='store_true', help='Só status + top-3 hot PCs')
    ap.add_argument('--compare', metavar='PREV_LOG', help='Compara com run anterior')
    args = ap.parse_args()

    text = Path(args.log).read_text(encoding='utf-8', errors='replace')
    r = parse_log(text)

    if args.short:
        status, emoji = verdict(r)
        print(f"{emoji} {status}  frames={r['frames']}  gs_writes={r['gs_writes']}")
        if r['hot_pcs']:
            print(f"  Top PC: {r['hot_pcs'][0][0]} ({r['hot_pcs'][0][2]:.0f}%)")
        return

    print_report(r)

    if args.compare:
        prev_text = Path(args.compare).read_text(encoding='utf-8', errors='replace')
        prev = parse_log(prev_text)
        compare_reports(prev, r)

if __name__ == '__main__':
    main()
