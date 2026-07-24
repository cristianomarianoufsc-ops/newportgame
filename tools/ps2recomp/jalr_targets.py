#!/usr/bin/env python3
"""
jalr_targets.py — Analisa jalr no output.c para entender padrões de dispatch indireto.

O que faz:
  1. Lista todos os call sites de ps2_dispatch (gerados pelo jalr)
  2. Tenta rastrear o valor do registrador fonte (por backward data-flow simples)
  3. Detecta funções que são APENAS chamadas via jalr (nunca via jal direto)
  4. Reporta funções com jalr não resolvível (r[2] dinâmico vs constante)

Uso:
  python3 jalr_targets.py build/output.c
  python3 jalr_targets.py build/output.c --unresolved   # foca nos não resolvíveis
  python3 jalr_targets.py build/output.c --top 20       # top N por frequência
"""
import re, sys, argparse
from collections import defaultdict, Counter

FUNC_RE     = re.compile(r'^void (func_[0-9a-f]+)\(PS2Regs\* regs\) \{')
DISPATCH_RE = re.compile(r'ps2_dispatch\(\(uint32_t\)regs->r\[(\d+)\]')
JAL_RE      = re.compile(r'func_([\da-f]+)\(regs\)')          # direct call
ASSIGN_RE   = re.compile(r'regs->r\[(\d+)\]\s*=\s*(.*?);')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', help='output.c gerado pelo recompilador')
    ap.add_argument('--unresolved', action='store_true', help='mostrar só call sites não resolvíveis')
    ap.add_argument('--top', type=int, default=30, help='top N funções com mais jalr')
    args = ap.parse_args()

    # Pass 1: coletar todos os callers diretos (jal) e indiretos (jalr)
    direct_calls  = set()   # endereços chamados diretamente
    jalr_sites    = []      # (caller_func, reg, context_lines)
    jalr_per_func = Counter()

    cur_func  = None
    buf       = []          # últimas N linhas da função atual

    with open(args.file, encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    for lineno, line in enumerate(lines):
        m = FUNC_RE.match(line)
        if m:
            cur_func = m.group(1)
            buf = []
            continue

        if cur_func is None:
            continue

        buf.append(line.rstrip())
        if len(buf) > 30:
            buf.pop(0)

        # Direct jal calls
        for cm in JAL_RE.finditer(line):
            direct_calls.add(cm.group(1))

        # Indirect jalr call sites
        dm = DISPATCH_RE.search(line)
        if dm:
            reg = int(dm.group(1))
            # Try to resolve: look backwards for last assignment to this reg
            resolved = None
            hex_re = re.compile(r'=\s*0x([0-9a-f]+)u', re.I)
            for prev in reversed(buf[:-1]):
                am = ASSIGN_RE.search(prev)
                if am and int(am.group(1)) == reg:
                    hm = hex_re.search(am.group(2))
                    if hm:
                        resolved = hm.group(1)
                    break

            jalr_sites.append({
                'func'    : cur_func,
                'reg'     : reg,
                'resolved': resolved,
                'context' : list(buf[-6:]),
                'lineno'  : lineno + 1,
            })
            jalr_per_func[cur_func] += 1

    # Pass 2: functions ONLY reached via jalr
    jal_only_indirect = set()
    for site in jalr_sites:
        if site['resolved'] and site['resolved'] not in direct_calls:
            jal_only_indirect.add(site['resolved'])

    # ---- Report ----
    print(f"\n=== jalr_targets.py — {args.file} ===\n")
    print(f"  Total jalr call sites : {len(jalr_sites):>6,}")
    print(f"  Resolvíveis (constante): {sum(1 for s in jalr_sites if s['resolved']):>6,}")
    print(f"  Não resolvíveis (dinâm): {sum(1 for s in jalr_sites if not s['resolved']):>6,}")
    print(f"  Funções só via jalr   : {len(jal_only_indirect):>6,}")
    print()

    if args.unresolved:
        print("  === Call sites NÃO resolvíveis (jalr via registrador dinâmico) ===\n")
        shown = 0
        for site in jalr_sites:
            if site['resolved']:
                continue
            print(f"  {site['func']}  linha {site['lineno']}  r[{site['reg']}]")
            for cl in site['context']:
                print(f"    {cl}")
            print()
            shown += 1
            if shown >= args.top:
                break
    else:
        print(f"  Top {args.top} funções por jalr call sites:")
        print(f"  {'Função':<28} {'jalr sites':>10}  {'resolvíveis':>12}")
        print("  " + "-"*55)
        for func, cnt in jalr_per_func.most_common(args.top):
            res = sum(1 for s in jalr_sites if s['func'] == func and s['resolved'])
            print(f"  {func:<28} {cnt:>10,}  {res:>12,}")

        if jal_only_indirect:
            print(f"\n  Funções atingidas APENAS via jalr ({len(jal_only_indirect)}):")
            for addr in sorted(jal_only_indirect)[:20]:
                print(f"    func_{addr}")
            if len(jal_only_indirect) > 20:
                print(f"    ... +{len(jal_only_indirect)-20} mais")

    print()

if __name__ == '__main__':
    main()
