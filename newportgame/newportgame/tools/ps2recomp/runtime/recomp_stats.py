#!/usr/bin/env python3
"""
recomp_stats.py — Estatísticas do output.c / output_runtime.c gerado.

Uso:
  python3 recomp_stats.py ../build/output.c
  python3 recomp_stats.py ../build/output_runtime.c
"""

import sys
import re
import os
from collections import Counter

def analyse(path: str):
    print(f"\n=== Estatísticas: {os.path.basename(path)} ===\n")

    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        src = f.read()

    lines = src.splitlines()
    print(f"  Linhas totais     : {len(lines):>8,}")
    print(f"  Tamanho           : {os.path.getsize(path)/1024:>8.1f} KB")

    # Funções geradas
    funcs = re.findall(r'^void (func_[0-9a-f]+)\(PS2Regs\*', src, re.MULTILINE)
    named = re.findall(r'^void ([A-Za-z_]\w+)\(PS2Regs\*', src, re.MULTILINE)
    named = [n for n in named if not n.startswith('func_') and n != 'ps2_game_start']

    print(f"\n  Funções anon.     : {len(funcs):>8,}  (func_XXXXXXXX)")
    print(f"  Funções nomeadas  : {len(named):>8,}  (símbolo ELF)")

    # Instruções por categoria
    categories = {
        'ALU'         : r'// (addiu|addi|addu|add|subu|sub|and|or|xor|nor|andi|ori|xori|lui|slt\w*)\b',
        'Shift'       : r'// (sll|srl|sra|sllv|srlv|srav|dsll|dsrl|dsra)\b',
        'Load'        : r'// (lw|lh|lhu|lb|lbu|ld|lq|lwl|lwr)\b',
        'Store'       : r'// (sw|sh|sb|sd|sq|swl|swr)\b',
        'Branch'      : r'// (beq|bne|blez|bgtz|bgez|bltz|beql|bnel|blezl|bgtzl|bgezl|bltzl|b)\b',
        'Jump'        : r'// (j|jal|jr|jalr)\b',
        'Multiply'    : r'// (mult|multu|madd\w*)\b',
        'Divide'      : r'// (div|divu)\b',
        'Float'       : r'// (add\.s|sub\.s|mul\.s|div\.s|mov\.s|cvt\.\w+)\b',
        'Move HI/LO'  : r'// (mfhi|mflo|mthi|mtlo|move)\b',
        'Syscall'     : r'ps2_syscall\(',
        'TODO'        : r'/\* TODO',
        'UNHANDLED'   : r'/\* UNHANDLED',
    }

    print(f"\n  Instruções por categoria:")
    total_instrs = 0
    for cat, pat in categories.items():
        count = len(re.findall(pat, src))
        if count:
            print(f"    {cat:<18}: {count:>8,}")
            total_instrs += count

    # TODOs and unhandled
    todos = re.findall(r'/\* TODO: (\w+)', src)
    todo_counter = Counter(todos)
    if todo_counter:
        print(f"\n  Top TODO mnemonics:")
        for mnem, cnt in todo_counter.most_common(10):
            print(f"    {mnem:<20}: {cnt:>6,}")

    unhandled = re.findall(r'/\* UNHANDLED: (\w[\w.]*)', src)
    uh_counter = Counter(unhandled)
    if uh_counter:
        print(f"\n  Top UNHANDLED mnemonics:")
        for mnem, cnt in uh_counter.most_common(10):
            print(f"    {mnem:<20}: {cnt:>6,}")

    # Funções maiores
    func_sizes = {}
    cur_func = None
    cur_count = 0
    for line in lines:
        m = re.match(r'^void (func_\w+|ps2_entry)\(PS2Regs\*', line)
        if m:
            if cur_func:
                func_sizes[cur_func] = cur_count
            cur_func = m.group(1)
            cur_count = 0
        if cur_func:
            cur_count += 1
    if cur_func:
        func_sizes[cur_func] = cur_count

    if func_sizes:
        top = sorted(func_sizes.items(), key=lambda x: -x[1])[:10]
        print(f"\n  10 maiores funções (linhas):")
        for name, sz in top:
            print(f"    {name:<30}: {sz:>6,}")

    print()

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Uso: python3 {sys.argv[0]} output.c")
        sys.exit(1)
    analyse(sys.argv[1])
