---
name: Tooling workflow
description: Sequência de ferramentas a usar antes e depois de cada mudança no runtime. Inclui regra anti-falso-positivo extraída da análise do projeto godofwar.
---

# Workflow de ferramentas ps2recomp

## Pré-commit (qualquer mudança em bios_stub/gs_stub/spu2_stub/host_main)

```bash
PYTHON=/nix/store/010r29jy64nj14dx7fabacypr4f2q077-python3-3.11.9-env/bin/python3
cd /home/runner/workspace/newportgame/newportgame
$PYTHON tools/ps2recomp/validate_patch.py          # deve retornar exit 0
```

## Build após mudança

```bash
bash tools/ps2recomp/build_relink.sh               # rápido (~10s) — não toca output_runtime.c
```

## Teste headless (requer ELF da ISO)

```bash
RT=tools/ps2recomp/runtime/build/ps2_game
ELF=tools/ps2recomp/build/elf_out/SCUS_973.99.elf
$RT --headless --frames 30 $ELF 2>&1 | tee /tmp/headless.log
$PYTHON tools/ps2recomp/triage_headless.py /tmp/headless.log
```

## Regra anti-falso-positivo (lição do godofwar)

**CRITÉRIO DE PROGRESSO REAL:** `frames_completados > 0` no output do triage_headless.py por código PS2 nativo.

- `frames = 0` → bloqueado; hot PC indica o suspeito → investigar, não mascarar
- Override via `override_stubs.c` apenas para funções **fora do range recompilado** (gaps)
- NUNCA force-retornar valor artificial sem entender o que a função faz no binário PS2
- O projeto godofwar desperdiçou ~300 PASSOs assim (função `0x35C1B0` fora do range, escrevia handler da state machine c838 → nunca chamada → c80C nunca escrito → boot travado)

**Why:** É tentador mascarar um bloqueio adicionando um override que retorna um valor qualquer. O resultado é `frames` estagnado enquanto parecem estar "avançando". O único critério de progresso real é frames por código PS2 nativo.

**How to apply:** Antes de adicionar qualquer override, rodar triage_headless.py e verificar `frames > 0`. Se frames = 0, o bloqueio é antes do primeiro frame — investigar o hot PC.

## Python path

```
/nix/store/010r29jy64nj14dx7fabacypr4f2q077-python3-3.11.9-env/bin/python3
```

`python3` pode não estar no PATH no Replit — usar o path acima explicitamente.
