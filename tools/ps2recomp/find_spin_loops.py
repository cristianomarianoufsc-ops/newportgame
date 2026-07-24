#!/usr/bin/env python3
"""
find_spin_loops.py — detecta funções no output.c que contêm syscalls dentro de loops.

Um spin-loop de syscall é o padrão mais comum de travamento no port:
o jogo faz syscall em loop esperando uma resposta do IOP que nunca chega.

Uso:
  python3 find_spin_loops.py build/output.c            # lista todos os spin-loops encontrados
  python3 find_spin_loops.py build/output.c --top 20   # top 20 por profundidade de loop
  python3 find_spin_loops.py build/output.c --show func_299390  # mostra detalhes de uma função
"""

import re, sys, argparse

def parse_args():
    p = argparse.ArgumentParser(description="Detecta syscalls dentro de loops no output.c")
    p.add_argument("output_c", help="Caminho para output.c")
    p.add_argument("--top", type=int, default=30, help="Top N funções (default 30)")
    p.add_argument("--show", default=None, help="Mostra detalhes de uma função específica")
    return p.parse_args()

def load_file(path):
    with open(path, 'r', errors='replace') as f:
        return f.readlines()

def parse_functions(lines):
    """Retorna lista de (func_name, start_line, end_line, [line_indices])"""
    funcs = []
    cur_name = None
    cur_start = 0
    cur_lines = []
    for i, line in enumerate(lines):
        m = re.match(r'^(static )?void (func_[0-9a-fA-F]+)\(PS2Regs\*', line)
        if m:
            if cur_name:
                funcs.append((cur_name, cur_start, i - 1, cur_lines))
            cur_name = m.group(2)
            cur_start = i
            cur_lines = [i]
        elif cur_name:
            cur_lines.append(i)
    if cur_name:
        funcs.append((cur_name, cur_start, len(lines) - 1, cur_lines))
    return funcs

def analyze_function(func_lines, all_lines):
    """
    Returns dict:
      has_loop: bool
      has_syscall: bool
      syscall_in_loop: bool
      syscall_codes: list[int]
      backward_gotos: list[str]   # label targets of backward gotos
      loop_depth: int             # how many nested loops enclose a syscall
    """
    labels_defined = set()
    labels_used = {}   # label -> [line_idx that goto it]
    syscall_lines_idx = []
    backward_gotos = []

    for i, li in enumerate(func_lines):
        line = all_lines[li]
        # Detect label definitions: "L_XXXXXX:"
        for m in re.finditer(r'\bL_([0-9a-fA-F]+)\s*:', line):
            labels_defined.add(m.group(1))
        # Detect gotos
        m = re.search(r'\bgoto\s+L_([0-9a-fA-F]+)\s*;', line)
        if m:
            lbl = m.group(1)
            labels_used.setdefault(lbl, []).append(i)
        # Detect syscall calls
        if 'ps2_syscall(regs,' in line:
            syscall_lines_idx.append(i)

    # Backward gotos: goto L_X where L_X is defined BEFORE the goto
    # In the func_lines order, a backward goto goes to a label that appears
    # earlier in the function.
    label_first_def = {}   # label -> first position in func_lines
    for i, li in enumerate(func_lines):
        line = all_lines[li]
        for m in re.finditer(r'\bL_([0-9a-fA-F]+)\s*:', line):
            lbl = m.group(1)
            if lbl not in label_first_def:
                label_first_def[lbl] = i

    backward_loop_labels = set()
    for lbl, positions in labels_used.items():
        def_pos = label_first_def.get(lbl, None)
        if def_pos is None:
            continue
        for pos in positions:
            if pos > def_pos:   # goto is AFTER the label → backward branch → loop
                backward_loop_labels.add(lbl)
                break

    # For each syscall, check if it's inside a backward loop region
    syscall_codes = []
    syscall_in_loop = False
    for si in syscall_lines_idx:
        # Look back for r[3] assignment
        for back in range(1, 25):
            idx = si - back
            if idx < 0: break
            line = all_lines[func_lines[idx]]
            m = re.search(r'regs->r\[3\]\s*=.*?(-?\d+|0x[0-9a-fA-F]+)', line)
            if m:
                raw = m.group(1)
                try:
                    val = int(raw, 16) if raw.startswith('0x') else int(raw)
                    val32 = val & 0xFFFFFFFF
                    if val32 > 0x7FFFFFFF:
                        val32 = -(0x100000000 - val32)
                    code = abs(val32)
                    if 0 < code < 0x200:
                        syscall_codes.append(code)
                except ValueError:
                    pass
                break

        # Check if this syscall position is inside any backward loop region
        for lbl in backward_loop_labels:
            def_pos = label_first_def.get(lbl, None)
            if def_pos is not None and def_pos <= si:
                # Find the goto position for this label
                for pos in labels_used.get(lbl, []):
                    if pos > def_pos and pos >= si:
                        syscall_in_loop = True
                        break

    return {
        "has_loop": len(backward_loop_labels) > 0,
        "has_syscall": len(syscall_lines_idx) > 0,
        "syscall_in_loop": syscall_in_loop,
        "syscall_codes": syscall_codes,
        "backward_gotos": list(backward_loop_labels),
        "loop_count": len(backward_loop_labels),
        "syscall_count": len(syscall_lines_idx),
    }

def main():
    args = parse_args()
    print(f"[*] Lendo {args.output_c} ...", flush=True)
    lines = load_file(args.output_c)
    print(f"[*] {len(lines)} linhas. Parseando funções ...", flush=True)
    funcs = parse_functions(lines)
    print(f"[*] {len(funcs)} funções encontradas. Analisando ...", flush=True)

    if args.show:
        target = args.show
        for (name, start, end, fl) in funcs:
            if name == target:
                print(f"\n=== {name} (linhas {start+1}-{end+1}) ===")
                for li in fl:
                    print(f"  {li+1:6d}: {lines[li]}", end="")
                return
        print(f"[!] Função '{target}' não encontrada.")
        return

    # Find spin-loops: functions with syscall inside a loop
    spin_loops = []
    for (name, start, end, fl) in funcs:
        info = analyze_function(fl, lines)
        if info["syscall_in_loop"]:
            spin_loops.append((name, start + 1, info))

    print(f"\n{'Função':25s}  {'Linha':>7}  {'Syscalls':>8}  {'Loops':>6}  {'Codes'}")
    print("-" * 75)
    for (name, lineno, info) in sorted(spin_loops, key=lambda x: -x[2]["loop_count"])[:args.top]:
        codes = ", ".join(f"0x{c:02x}" for c in info["syscall_codes"])
        print(f"  {name:25s}  {lineno:7d}  {info['syscall_count']:8d}  {info['loop_count']:6d}  {codes}")

    print(f"\nTotal: {len(spin_loops)} funções com syscall dentro de loop.")

if __name__ == "__main__":
    main()
