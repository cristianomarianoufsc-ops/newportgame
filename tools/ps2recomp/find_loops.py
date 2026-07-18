#!/usr/bin/env python3
"""
find_loops.py — identifica funções com backward gotos (loops) no output.c gerado.
Uso: python3 find_loops.py build/output.c [--top N] [--min-back N]
"""
import re, sys, argparse
from collections import defaultdict

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", help="output.c gerado pelo recompilador")
    ap.add_argument("--top", type=int, default=30, help="mostrar top N funções por backward gotos")
    ap.add_argument("--min-back", type=int, default=1, help="min backward gotos para listar")
    ap.add_argument("--func", help="mostrar detalhes de uma função específica (ex: func_2998c0)")
    args = ap.parse_args()

    func_re    = re.compile(r'^void (func_\w+)\(PS2Regs\* regs\) \{')
    label_re   = re.compile(r'^(L_[0-9a-f]+):')
    goto_re    = re.compile(r'\bgoto (L_[0-9a-f]+)\b')
    syscall_re = re.compile(r'ps2_syscall\(regs,')
    call_re    = re.compile(r'\bfunc_([0-9a-f]+)\(regs\)')

    functions = {}   # name → {labels, gotos, line_of_label, line_of_goto, syscalls}
    cur = None
    label_lines = {}  # label → line number within function

    with open(args.file, encoding='utf-8', errors='replace') as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip()
            m = func_re.match(line)
            if m:
                cur = m.group(1)
                functions[cur] = {
                    'labels': {},      # label → line-in-func
                    'gotos':  [],      # (label, line-in-func)
                    'syscalls': 0,
                    'calls': set(),
                    'start_line': lineno,
                }
                label_lines = {}
                func_lineno = lineno
                continue

            if cur is None:
                continue

            rel = lineno - func_lineno

            m = label_re.match(line)
            if m:
                lbl = m.group(1)
                functions[cur]['labels'][lbl] = rel
                label_lines[lbl] = rel
                continue

            m = goto_re.search(line)
            if m:
                functions[cur]['gotos'].append((m.group(1), rel))

            if syscall_re.search(line):
                functions[cur]['syscalls'] += 1

            for cm in call_re.finditer(line):
                functions[cur]['calls'].add('func_' + cm.group(1))

    # Compute backward gotos per function
    results = []
    for name, info in functions.items():
        backward = []
        for (lbl, goto_line) in info['gotos']:
            label_line = info['labels'].get(lbl)
            if label_line is not None and label_line <= goto_line:
                backward.append((lbl, label_line, goto_line))
        info['backward'] = backward
        if len(backward) >= args.min_back:
            results.append((len(backward), info['syscalls'], name, info))

    results.sort(reverse=True)

    if args.func:
        # Detailed view for one function
        name = args.func
        info = functions.get(name)
        if not info:
            print(f"Função '{name}' não encontrada.")
            sys.exit(1)
        print(f"\n=== {name} (linha {info['start_line']}) ===")
        print(f"  Labels:          {len(info['labels'])}")
        print(f"  Gotos:           {len(info['gotos'])}")
        print(f"  Backward gotos:  {len(info['backward'])}")
        print(f"  Syscalls:        {info['syscalls']}")
        print(f"  Calls:           {', '.join(sorted(info['calls']))}")
        if info['backward']:
            print("  Backward goto details (label_line → goto_line):")
            for lbl, ll, gl in info['backward']:
                print(f"    {lbl}  label@+{ll}  goto@+{gl}  (span={gl-ll} lines)")
        return

    print(f"\n{'Rank':<5} {'Função':<25} {'Back-gotos':<12} {'Syscalls':<10}")
    print("-" * 55)
    for rank, (back_cnt, sysc, name, info) in enumerate(results[:args.top], 1):
        print(f"{rank:<5} {name:<25} {back_cnt:<12} {sysc:<10}")
        for lbl, ll, gl in info['backward'][:3]:
            print(f"       ↺ {lbl}  +{ll}→+{gl}  (span {gl-ll})")

    print(f"\nTotal funções analisadas: {len(functions)}")

if __name__ == '__main__':
    main()
