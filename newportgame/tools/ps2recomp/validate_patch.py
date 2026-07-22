#!/usr/bin/env python3
"""
validate_patch.py — Validação pré-commit para ps2recomp
========================================================
Detecta erros comuns em bios_stub.cpp, recompiler.cpp e runtime sources
antes de commitar:

  1. Chaves desbalanceadas { }
  2. Parênteses desbalanceados ( ) — só .cpp/.h
  3. Newlines literais em string literals C/C++
     (respeitando raw strings R"...(...)..." e line-continuation \\)
  4. TODO/UNHANDLED residuais (aviso, não erro)

REGRA ANTI-FALSO-POSITIVO (análoga ao C838-GUARD NATIVOS do godofwar):
  Um stub que force-retorna valor sem executar código PS2 nativo não é
  progresso. Critério de progresso real:
    triage_headless.py → frames_completados > 0 por código PS2 nativo.

Uso:
  python3 tools/ps2recomp/validate_patch.py                  # verifica todos
  python3 tools/ps2recomp/validate_patch.py src/bios_stub.cpp
  python3 tools/ps2recomp/validate_patch.py --strict         # exit 2 em avisos
  python3 tools/ps2recomp/validate_patch.py --verbose        # lista TODOs

Retorna: exit 0 = OK | exit 1 = erro crítico | exit 2 = avisos (--strict).
"""
import re, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent

DEFAULT_FILES = [
    "tools/ps2recomp/runtime/src/bios_stub.cpp",
    "tools/ps2recomp/runtime/src/gs_stub.cpp",
    "tools/ps2recomp/runtime/src/host_main.cpp",
    "tools/ps2recomp/runtime/src/spu2_stub.cpp",
    "tools/ps2recomp/src/recomp/recompiler.cpp",
    "tools/ps2recomp/src/mips/disasm.cpp",
]

SEP = "=" * 60

# ── Strip helpers ─────────────────────────────────────────────────

def strip_block_comments(text: str) -> str:
    """Remove /* ... */ preservando newlines para manter contagem de linhas."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i:i+2] == '/*':
            i += 2
            while i < n and text[i:i+2] != '*/':
                if text[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
        else:
            out.append(text[i]); i += 1
    return ''.join(out)

def strip_raw_strings(text: str) -> str:
    """
    Substitui raw string literals  R"delim(...)delim"  por espaços
    (preservando newlines para não deslocar números de linha).
    Raw strings são válidas em C++11 e podem conter qualquer coisa — não
    devem ser analisadas para chaves, parênteses ou strings comuns.
    """
    # Padrão: R"delim( ... )delim"   (delim pode ser vazio ou até 16 chars)
    def replace(m: re.Match) -> str:
        s = m.group(0)
        return ''.join('\n' if c == '\n' else ' ' for c in s)
    return re.sub(r'R"([^(\s]*)\(.*?\)\1"', replace, text, flags=re.DOTALL)

# ── Balanceamento ─────────────────────────────────────────────────

def check_balanced(raw: str, open_c: str, close_c: str, fname: str) -> list[str]:
    """Verifica balanceamento ignorando /* */, raw strings e comentários //."""
    text = strip_raw_strings(strip_block_comments(raw))
    errors, depth = [], 0
    for lineno, line in enumerate(text.split('\n'), 1):
        in_str = False
        i = 0
        while i < len(line):
            c = line[i]
            if in_str:
                if c == '\\': i += 1              # escape
                elif c == '"': in_str = False
            else:
                if c == '"':
                    in_str = True
                elif c == '/' and line[i+1:i+2] == '/':
                    break                          # comentário de linha
                elif c == open_c:
                    depth += 1
                elif c == close_c:
                    depth -= 1
                    if depth < 0:
                        errors.append(f"{fname}:{lineno} — '{close_c}' extra (depth<0)")
                        depth = 0
            i += 1
    if depth:
        errors.append(f"{fname} — {depth} '{open_c}' não fechado(s) ao fim do arquivo")
    return errors

# ── Newlines literais em strings ──────────────────────────────────

def check_literal_newlines(raw: str, fname: str) -> list[str]:
    """
    Detecta string literals que abrem numa linha e não fecham (newline literal).
    Ignora: /* */, raw strings R"...", line-continuation (\\), strings #include.
    """
    # Remove blocos que enganam o parser
    text = strip_raw_strings(strip_block_comments(raw))
    errors = []

    for lineno, line in enumerate(text.split('\n'), 1):
        stripped = line.rstrip()

        # line-continuation — string pode cruzar linhas (macros)
        if stripped.endswith('\\'):
            continue

        in_str = False
        i = 0
        while i < len(stripped):
            c = stripped[i]
            if in_str:
                if c == '\\': i += 1   # escape char
                elif c == '"': in_str = False
            else:
                if c == '"':
                    in_str = True
                elif c == '/' and stripped[i+1:i+2] == '/':
                    break   # comentário de linha
            i += 1

        if in_str:
            errors.append(f"{fname}:{lineno} — string não fechada antes do EOL (newline literal)")
    return errors

# ── TODOs ─────────────────────────────────────────────────────────

def check_todos(raw: str, fname: str) -> list[str]:
    pat = re.compile(r'/\*\s*(TODO|UNHANDLED)\s*:', re.IGNORECASE)
    return [
        f"{fname}:{lineno} — {m.group(1)}: {line.strip()[:70]}"
        for lineno, line in enumerate(raw.split('\n'), 1)
        for m in [pat.search(line)] if m
    ]

# ── Por arquivo ───────────────────────────────────────────────────

def validate_file(path: Path) -> tuple[int, int]:
    if not path.exists():
        print(f"  [SKIP] {path.name}")
        return 0, 0
    raw = path.read_text(encoding='utf-8', errors='replace')
    errors, warnings = [], []

    errors   += check_balanced(raw, '{', '}', path.name)
    if path.suffix in ('.cpp', '.h'):
        errors += check_balanced(raw, '(', ')', path.name)
    errors   += check_literal_newlines(raw, path.name)
    warnings += check_todos(raw, path.name)

    if errors or warnings:
        print(f"\n  {path.name}:")
        for e in errors:   print(f"    ❌ {e}")
        if warnings:       print(f"    ⚠️  {len(warnings)} TODO/UNHANDLED")
    else:
        print(f"  ✅ {path.name}")
    return len(errors), len(warnings)

# ── main ──────────────────────────────────────────────────────────

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--strict',  action='store_true')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    paths = [Path(f) for f in args.files] if args.files else [REPO / f for f in DEFAULT_FILES]

    print(SEP)
    print("validate_patch.py — ps2recomp pré-commit")
    print(SEP)

    te = tw = 0
    for p in paths:
        e, w = validate_file(p)
        te += e; tw += w
        if args.verbose and w:
            raw = p.read_text(encoding='utf-8', errors='replace')
            for msg in check_todos(raw, p.name):
                print(f"      ⚠️  {msg}")

    print(f"\n{SEP}")
    print(f"  Erros   : {te}")
    print(f"  Avisos  : {tw}")
    print(SEP)
    if te:
        print("❌ FALHOU — corrigir antes de commitar."); sys.exit(1)
    if args.strict and tw:
        print("⚠️  Avisos (--strict)."); sys.exit(2)
    print("✅ OK — pode commitar.")

if __name__ == '__main__':
    main()
