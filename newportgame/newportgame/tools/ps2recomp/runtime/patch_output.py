#!/usr/bin/env python3
"""
patch_output.py — Prepara o output.c gerado pelo recompilador para compilar
                  junto com o runtime ps2_game (host_main + gs_stub + bios_stub).

O que faz:
  1. Remove o header inline (PS2Regs, ps2_ram, etc.) e substitui por
     #include "ps2_runtime.h"  (que fornece as definições corretas com extern)
  2. Renomeia  int main(void)  →  void ps2_game_start(void)
     e remove o "return 0;" final (void não retorna int)
  3. Salva o resultado em output_runtime.c (nunca sobrescreve output.c)

Uso:
  python3 patch_output.py ../build/output.c [../build/output_runtime.c]
"""

import sys
import re
import os

# -----------------------------------------------------------------------
# Marcadores que delimitam o bloco de header gerado pelo recompilador
# -----------------------------------------------------------------------
HEADER_START = "// ps2recomp generated output"
FWD_DECL     = "// Forward declarations"

# -----------------------------------------------------------------------
# patch()
# -----------------------------------------------------------------------
def patch(src: str) -> str:
    lines_in = src.count('\n')

    # ------------------------------------------------------------------
    # 1. Substituir bloco de header inline por #include "ps2_runtime.h"
    # ------------------------------------------------------------------
    si = src.find(HEADER_START)
    ei = src.find(FWD_DECL)

    if si == -1 or ei == -1:
        print(f"  AVISO: marcadores de header não encontrados "
              f"(si={si}, ei={ei}) — pulando patch do header")
    else:
        new_header = (
            "// ps2recomp generated output — patched for runtime\n"
            "// DO NOT EDIT — edite o original output.c e rode patch_output.py\n"
            "\n"
            "#include \"ps2_runtime.h\"\n"
            "\n"
        )
        src = new_header + src[ei:]
        print(f"  Header substituído por #include \"ps2_runtime.h\"")

    # ------------------------------------------------------------------
    # 2. Renomear  int main(void)  →  void ps2_game_start(void)
    # ------------------------------------------------------------------
    old_sig = "int main(void) {"
    new_sig  = "void ps2_game_start(void) {"

    if old_sig in src:
        src = src.replace(old_sig, new_sig, 1)
        print(f"  main() → ps2_game_start()")

        # Remove  "    return 0;\n"  que está dentro do antigo main
        # (última ocorrência antes do fechamento final "}")
        src = re.sub(r'\n    return 0;\n\}(\s*)$', '\n}\n', src)
        print(f"  'return 0;' removido de ps2_game_start")
    else:
        print("  AVISO: 'int main(void)' não encontrado — verifique o output.c")

    lines_out = src.count('\n')
    print(f"  Linhas: {lines_in} → {lines_out}  ({lines_out - lines_in:+d})")
    return src


# -----------------------------------------------------------------------
# main
# -----------------------------------------------------------------------
def main():
    if len(sys.argv) < 2:
        script = os.path.basename(sys.argv[0])
        print(f"Uso: python3 {script} input.c [output_runtime.c]")
        sys.exit(1)

    in_path = sys.argv[1]
    if len(sys.argv) >= 3:
        out_path = sys.argv[2]
    else:
        base = os.path.splitext(in_path)[0]
        out_path = base + "_runtime.c"

    if not os.path.isfile(in_path):
        print(f"ERRO: arquivo não encontrado: {in_path}")
        sys.exit(1)

    print(f"patch_output.py")
    print(f"  entrada : {in_path}")
    print(f"  saída   : {out_path}")

    with open(in_path, 'r', encoding='utf-8', errors='replace') as f:
        src = f.read()

    patched = patch(src)

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(patched)

    size_kb = os.path.getsize(out_path) / 1024
    print(f"  Escrito: {out_path}  ({size_kb:.1f} KB)")
    print("Patch concluído.")


if __name__ == '__main__':
    main()
