#!/usr/bin/env python3
"""
analyze_syscall.py — encontra call sites de syscalls no output.c gerado.

Uso:
  python3 analyze_syscall.py build/output.c             # lista todos os syscalls e contagens
  python3 analyze_syscall.py build/output.c 0x83        # mostra contexto do syscall 0x83
  python3 analyze_syscall.py build/output.c 0x83 --ctx 30  # contexto maior
"""

import re, sys

def parse_args():
    import argparse
    p = argparse.ArgumentParser(description="Análise de syscalls no output.c recompilado")
    p.add_argument("output_c", help="Caminho para output.c")
    p.add_argument("syscall", nargs="?", default=None, help="Número do syscall (hex ou decimal)")
    p.add_argument("--ctx", type=int, default=15, help="Linhas de contexto por chamada (default 15)")
    p.add_argument("--max", type=int, default=5, help="Máximo de call sites a mostrar")
    return p.parse_args()

def load_file(path):
    with open(path, 'r', errors='replace') as f:
        return f.readlines()

def find_syscall_map(lines):
    """Retorna dict: {syscall_code -> [line_numbers (1-based)]}"""
    syscall_lines = [i for i, l in enumerate(lines) if 'ps2_syscall(regs,' in l]
    results = {}
    for lineno in syscall_lines:
        found = False
        for back in range(1, 30):
            idx = lineno - back
            if idx < 0: break
            line = lines[idx]
            # Detect function boundary — stop searching backwards
            if re.match(r'^(static )?void func_', line):
                break
            # Match r[3] = some_integer_constant
            m = re.search(r'regs->r\[3\]\s*=\s*\(?[^;]*?(-?\d+|0x[0-9a-fA-F]+)\)?;', line)
            if m:
                raw = m.group(1)
                try:
                    val = int(raw, 16) if raw.startswith('0x') else int(raw)
                except ValueError:
                    continue
                # PS2 negative syscall convention: sign-extend 32-bit
                val32 = val & 0xFFFFFFFF
                if val32 > 0x7FFFFFFF:
                    val32 = -(0x100000000 - val32)
                code = abs(val32)
                if code > 0 and code < 0x200:  # plausible syscall range
                    if code not in results:
                        results[code] = []
                    results[code].append(lineno + 1)
                    found = True
                    break
        if not found:
            # Syscall with unknown code (e.g. computed at runtime)
            results.setdefault(0xFFFF, []).append(lineno + 1)
    return results

def show_summary(syscall_map):
    print(f"{'Syscall':>10}  {'Dec':>5}  {'Calls':>8}")
    print("-" * 30)
    for code in sorted(k for k in syscall_map if k != 0xFFFF):
        print(f"  0x{code:04x}    {code:5d}   {len(syscall_map[code]):8d}")
    if 0xFFFF in syscall_map:
        print(f"  0xFFFF (dynamic/unknown)   {len(syscall_map[0xFFFF]):8d}")

def show_context(lines, syscall_map, code, ctx_lines, max_sites):
    if code not in syscall_map:
        print(f"[!] Syscall 0x{code:x} não encontrado no mapa.")
        return
    sites = syscall_map[code]
    print(f"\n=== Syscall 0x{code:02x} ({code}) — {len(sites)} call sites ===\n")
    for i, lineno in enumerate(sites[:max_sites]):
        idx = lineno - 1  # 0-based
        start = max(0, idx - ctx_lines)
        end   = min(len(lines), idx + ctx_lines + 1)
        print(f"--- Call site {i+1} (line {lineno}) ---")
        for j in range(start, end):
            marker = ">>> " if j == idx else "    "
            print(f"{marker}{j+1:6d}: {lines[j]}", end="")
        print()

def main():
    args = parse_args()
    print(f"[*] Lendo {args.output_c} ...", flush=True)
    lines = load_file(args.output_c)
    print(f"[*] {len(lines)} linhas. Mapeando syscalls ...", flush=True)
    syscall_map = find_syscall_map(lines)

    if args.syscall is None:
        show_summary(syscall_map)
    else:
        code = int(args.syscall, 0)
        show_context(lines, syscall_map, code, args.ctx, args.max)

if __name__ == "__main__":
    main()
