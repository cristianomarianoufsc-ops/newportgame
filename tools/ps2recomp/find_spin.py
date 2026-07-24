#!/usr/bin/env python3
"""
find_spin.py — Ferramenta de diagnóstico de hot-PC para o recompilador PS2.

Uso:
    python3 find_spin.py <output.c> <PC_hex> [--callers N]

Exemplos:
    python3 find_spin.py build/output.c 0x220730
    python3 find_spin.py build/output.c 0x220730 --callers 3

Saída:
    - Função que contém o PC
    - Código C da função (com destaque na linha do PC)
    - Todos os call sites (quem chama essa função)
    - Cadeia de callers até N níveis acima
"""

import sys
import re
import argparse

def parse_args():
    p = argparse.ArgumentParser(description="Diagnóstico de hot-PC em output.c")
    p.add_argument("output_c", help="Caminho para output.c")
    p.add_argument("pc", help="Endereço PS2 em hex (ex: 0x220730)")
    p.add_argument("--callers", type=int, default=2,
                   help="Quantos níveis de callers mostrar (padrão: 2)")
    p.add_argument("--ctx", type=int, default=10,
                   help="Linhas de contexto antes/depois do PC (padrão: 10)")
    return p.parse_args()

def load_file(path):
    with open(path, "r", errors="replace") as f:
        return f.readlines()

def find_function_at_pc(lines, pc):
    """Retorna (func_name, start_line, end_line) da função que contém pc."""
    pc_hex = f"0x{pc:08x}u"
    pc_hex_alt = f"0x{pc:x}u"
    pc_comment = f"// {pc:08x}"

    func_start = None
    func_name = None
    func_start_line = -1

    for i, line in enumerate(lines):
        # Detecta início de função
        m = re.match(r'^(void|int|uint32_t)\s+(func_[0-9a-fA-F]+)\s*\(', line)
        if m:
            func_start = i
            func_name = m.group(2)
            func_start_line = i

        # Detecta PC na linha
        if func_start is not None:
            if (pc_hex in line or pc_hex_alt in line or pc_comment in line):
                # Encontrou — agora procura o fim da função
                end_line = len(lines)
                for j in range(i + 1, len(lines)):
                    m2 = re.match(r'^(void|int|uint32_t)\s+(func_[0-9a-fA-F]+)\s*\(', lines[j])
                    if m2:
                        end_line = j
                        break
                return func_name, func_start_line, end_line, i

    return None, -1, -1, -1

def find_callers(lines, func_name, max_results=20):
    """Retorna lista de (caller_func, call_site_line_no, line_content)."""
    callers = []
    pattern = re.compile(rf'\b{re.escape(func_name)}\s*\(')
    current_func = None

    for i, line in enumerate(lines):
        m = re.match(r'^(void|int|uint32_t)\s+(func_[0-9a-fA-F]+)\s*\(', line)
        if m:
            current_func = m.group(2)
        if current_func == func_name:
            continue  # não conta auto-referências de protótipo
        if pattern.search(line) and current_func and current_func != func_name:
            callers.append((current_func, i + 1, line.rstrip()))
            if len(callers) >= max_results:
                break

    return callers

def show_function(lines, func_name, start, end, hit_line, ctx):
    """Imprime a função com destaque na linha do PC."""
    print(f"\n{'='*70}")
    print(f"FUNÇÃO: {func_name}  (linhas {start+1}–{end})")
    print(f"{'='*70}")

    lo = max(start, hit_line - ctx)
    hi = min(end, hit_line + ctx + 1)

    if lo > start + 1:
        print(f"  ... ({lo - start - 1} linhas omitidas) ...")

    for i in range(lo, hi):
        marker = " >>>" if i == hit_line else "    "
        print(f"{marker} {i+1:6d}: {lines[i]}", end="")

    if hi < end:
        print(f"  ... ({end - hi} linhas omitidas) ...")

def main():
    args = parse_args()
    try:
        pc = int(args.pc, 16)
    except ValueError:
        print(f"ERRO: PC inválido: {args.pc!r}")
        sys.exit(1)

    print(f"Carregando {args.output_c} ...", flush=True)
    lines = load_file(args.output_c)
    print(f"  {len(lines):,} linhas carregadas.", flush=True)

    # 1. Encontrar função que contém o PC
    func_name, start, end, hit_line = find_function_at_pc(lines, pc)
    if func_name is None:
        print(f"\nPC 0x{pc:08x} não encontrado em {args.output_c}")
        print("Dica: verifique se o PC está no range recompilado.")
        sys.exit(1)

    print(f"\nPC 0x{pc:08x} encontrado em: {func_name} (linha {hit_line+1})")
    show_function(lines, func_name, start, end, hit_line, args.ctx)

    # 2. Callers imediatos
    callers = find_callers(lines, func_name)
    print(f"\n{'='*70}")
    print(f"CALLERS diretos de {func_name}: {len(callers)} encontrado(s)")
    print(f"{'='*70}")
    for caller, lineno, content in callers:
        print(f"  {caller}  (linha {lineno})")
        print(f"    {content.strip()}")

    # 3. Cadeia de callers (BFS, N níveis)
    if args.callers > 1 and callers:
        print(f"\n{'='*70}")
        print(f"CADEIA de callers ({args.callers} níveis):")
        print(f"{'='*70}")
        seen = {func_name}
        current_level = list({c[0] for c in callers})
        for level in range(2, args.callers + 1):
            next_level = []
            for fn in current_level:
                if fn in seen:
                    continue
                seen.add(fn)
                lvl_callers = find_callers(lines, fn, max_results=5)
                caller_names = [c[0] for c in lvl_callers]
                print(f"  Nível {level}: {fn}")
                for cn in caller_names:
                    print(f"    ← {cn}")
                next_level.extend(caller_names)
            current_level = list(set(next_level) - seen)
            if not current_level:
                break

    print()

if __name__ == "__main__":
    main()
