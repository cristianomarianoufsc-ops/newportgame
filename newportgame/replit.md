# Newport Game — PS2 Static Recompiler Port

Port nativo de PS2 para PC via recompilação estática de MIPS R5900 → C → x86-64.

> **Agentes: leia `AGENTS.md` antes de trabalhar neste projeto.**

## Run & Operate

- `bash tools/ps2recomp/build.sh` — compilar o recompilador (requer cmake + g++ + make)
- `cd tools/ps2recomp/build && ./ps2recomp recomp "God of War (USA).iso" output.c` — rodar recompilação
- `python3 tools/ps2recomp/runtime/recomp_stats.py build/output.c` — ver estatísticas do output
- `python3 tools/ps2recomp/find_loops.py build/output.c` — identificar funções com loops
- `python3 tools/ps2recomp/runtime/patch_output.py build/output.c` — preparar para o runtime

## Stack

- Recompilador: C++ (CMake), targets x86-64 Linux
- Python 3.12 — ferramentas de análise e pós-processamento
- pnpm workspaces, Node.js 24, TypeScript 5.9 (infraestrutura web)

## Ferramentas Python (ver `AGENTS.md` para detalhes)

| Arquivo | Função |
|---|---|
| `tools/ps2recomp/find_loops.py` | Detecta loops (backward gotos) no output.c |
| `tools/ps2recomp/runtime/patch_output.py` | Prepara output.c para compilar com o runtime |
| `tools/ps2recomp/runtime/recomp_stats.py` | Estatísticas do output.c gerado |

## Ambiente

- Sistema: NixOS (Replit) — não usar `apt`
- Python 3.12 instalado (`python3` disponível)
- cmake: instalar via `nix-env -iA nixpkgs.cmake` se necessário
- ISO do jogo: não versionada no git (8.52 GB)

## Gotchas

- Não rodar `apt install` — o ambiente é NixOS, usar `nix-env` ou skill de pacotes do Replit
- A ISO não está no git (`.gitignore`) — precisa ser baixada separadamente
- `cmake` pode não estar no PATH após instalar via `nix-env`; verificar com `which cmake`

## Pointers

- Ver `AGENTS.md` para fluxo completo e documentação das ferramentas Python
- Ver `tools/ps2recomp/README.md` para detalhes do recompilador
