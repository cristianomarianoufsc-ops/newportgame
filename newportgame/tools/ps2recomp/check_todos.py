#!/usr/bin/env python3
"""
check_todos.py — Varre o output.c e mostra quais mnemonics ainda são TODO/UNHANDLED.
Útil para decidir o que implementar a seguir no recompilador.

Uso:
  python3 check_todos.py build/output.c
  python3 check_todos.py build/output.c --min 10        # só mostra >= 10 ocorrências
  python3 check_todos.py build/output.c --category      # agrupa por categoria
"""
import re, sys, argparse
from collections import Counter

TODO_RE     = re.compile(r'/\*\s*TODO(?:: branch| jump)?: (\S+)')
UNHANDLED_RE= re.compile(r'/\*\s*UNHANDLED: (\S+)')

CATEGORY_HINTS = {
    # 64-bit ALU
    'daddi':'64bit-ALU', 'daddiu':'64bit-ALU', 'daddu':'64bit-ALU', 'dadd':'64bit-ALU',
    'dsubu':'64bit-ALU', 'dsub':'64bit-ALU',
    # 64-bit shift
    'dsll':'64bit-shift','dsrl':'64bit-shift','dsra':'64bit-shift',
    'dsll32':'64bit-shift','dsrl32':'64bit-shift','dsra32':'64bit-shift',
    'dsllv':'64bit-shift','dsrlv':'64bit-shift','dsrav':'64bit-shift',
    # Load/store
    'ld':'load64','lwc1':'fpu-load','lq':'load128','lwu':'load32u',
    'ldl':'load64','ldr':'load64','lqc2':'vu0-load',
    'sd':'store64','swc1':'fpu-store','sq':'store128',
    'sdl':'store64','sdr':'store64','sqc2':'vu0-store',
    # Moves
    'movz':'move','movn':'move',
    # Indirect call
    'jalr':'indirect-call',
    # FPU
    'add.s':'fpu-arith','sub.s':'fpu-arith','mul.s':'fpu-arith','div.s':'fpu-arith',
    'mov.s':'fpu-move','neg.s':'fpu-arith','abs.s':'fpu-arith',
    'cvt.s.w':'fpu-cvt','cvt.w.s':'fpu-cvt','cvt.d.s':'fpu-cvt',
    'c.lt.s':'fpu-cmp','c.le.s':'fpu-cmp','c.eq.s':'fpu-cmp',
    'bc1t':'fpu-branch','bc1f':'fpu-branch','bc1tl':'fpu-branch','bc1fl':'fpu-branch',
    'mfc1':'fpu-move','mtc1':'fpu-move','cfc1':'fpu-ctl','ctc1':'fpu-ctl',
    # MMI / VU0
    'pmfhi':'mmi','pmflo':'mmi','pmthi':'mmi','pmtlo':'mmi',
    'pcpyld':'mmi','pcpyud':'mmi','pcpyh':'mmi',
    'paddw':'mmi','psubw':'mmi','paddh':'mmi','psubh':'mmi',
    'qmfc2':'vu0','qmtc2':'vu0','vsub':'vu0','vadd':'vu0','vmul':'vu0',
    # Misc
    'sync':'misc','cache':'misc','pref':'misc','break':'misc',
    'tge':'trap','tgeu':'trap','tlt':'trap','tltu':'trap','teq':'trap','tne':'trap',
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', help='output.c gerado pelo recompilador')
    ap.add_argument('--min', type=int, default=1, help='mínimo de ocorrências para exibir')
    ap.add_argument('--category', action='store_true', help='agrupar por categoria')
    args = ap.parse_args()

    todos     = Counter()
    unhandled = Counter()
    with open(args.file, encoding='utf-8', errors='replace') as f:
        for line in f:
            for m in TODO_RE.finditer(line):
                todos[m.group(1)] += 1
            for m in UNHANDLED_RE.finditer(line):
                unhandled[m.group(1)] += 1

    total_todo = sum(todos.values())
    total_uh   = sum(unhandled.values())
    print(f"\n=== TODO/UNHANDLED em {args.file} ===")
    print(f"  TODO total      : {total_todo:>8,}")
    print(f"  UNHANDLED total : {total_uh:>8,}")
    print(f"  Mnemonics únicos: {len(todos) + len(unhandled):>8,}\n")

    all_counts = Counter()
    all_counts.update(todos)
    all_counts.update(unhandled)

    if args.category:
        by_cat = {}
        for mnem, cnt in all_counts.items():
            cat = CATEGORY_HINTS.get(mnem, 'other')
            by_cat.setdefault(cat, []).append((mnem, cnt))

        for cat in sorted(by_cat, key=lambda c: -sum(v for _,v in by_cat[c])):
            cat_total = sum(v for _,v in by_cat[cat])
            if cat_total < args.min:
                continue
            print(f"  [{cat}]  total={cat_total:,}")
            for mnem, cnt in sorted(by_cat[cat], key=lambda x: -x[1]):
                if cnt >= args.min:
                    tag = 'UNH' if mnem in unhandled and mnem not in todos else 'TODO'
                    print(f"    {tag}  {mnem:<22}: {cnt:>6,}")
    else:
        print(f"  {'Mnemonic':<24} {'TODO':>8}  {'UNHANDLED':>10}  {'Total':>8}  Categoria")
        print("  " + "-"*70)
        for mnem, total in all_counts.most_common():
            if total < args.min:
                break
            t  = todos.get(mnem, 0)
            uh = unhandled.get(mnem, 0)
            cat = CATEGORY_HINTS.get(mnem, '?')
            print(f"  {mnem:<24} {t:>8,}  {uh:>10,}  {total:>8,}  {cat}")

    print()

if __name__ == '__main__':
    main()
