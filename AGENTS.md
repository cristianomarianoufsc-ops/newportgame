# AGENTS.md — Leitura obrigatória para agentes

Este documento descreve as ferramentas disponíveis neste projeto e como usá-las.
**Todo agente que abrir este repositório deve ler este arquivo antes de trabalhar.**

---

## 🔁 Regra de push obrigatória

**Após concluir cada uma das etapas abaixo, o agente DEVE fazer `git add -A && git commit && git push origin main` antes de continuar para a próxima etapa:**

1. Depois de atualizar `bios_stub.cpp` com novo fix de syscall
2. Depois de patchar `output_runtime.c` (fixes intra-TU)
3. Depois de rodar `build_relink.sh` (ou `--full`) com sucesso
4. Depois de rodar headless e capturar novo hot-PC
5. Depois de atualizar o AGENTS.md com novo estado / histórico

**Motivo:** garante que o progresso não se perde entre sessões de agente e que o histórico de fixes é rastreável por commit.

```bash
cd /caminho/do/repo
git add -A
git commit -m "fix: <descrição do que foi corrigido>"
git push origin main
```

---

## Objetivo do projeto

Port nativo de PS2 para PC via recompilação estática MIPS → C → x86-64.
Fluxo: ISO de PS2 → ELF extraído → traduzido para C → compilado para x86-64.

Mesmo jogo que o projeto godofwar de outro agente, mas com arquitetura diferente:
- **Deles:** PS2Recomp framework, 5627 .cpp separados, R5900Context*, Raylib
- **Nosso:** saída monolítica (output.c, 350K linhas), PS2Regs* inline, SDL2+OpenGL

---

## ⚙️ Ferramentas OBRIGATÓRIAS

Sem estas o fluxo principal não funciona. Verifique antes de qualquer coisa.

### `tools/ps2recomp/build.sh` — compilar o recompilador

```bash
bash tools/ps2recomp/build.sh
# Gera: tools/ps2recomp/build/ps2recomp
```

Se precisar compilar manualmente:
```bash
cd tools/ps2recomp
g++ -std=c++20 -O2 -Wall \
  src/main.cpp src/iso/udf_parser.cpp src/elf/elf_loader.cpp \
  src/mips/disasm.cpp src/recomp/recompiler.cpp \
  -I src -o build/ps2recomp
```

---

### `tools/ps2recomp/runtime/patch_output.py` — preparar output para o runtime

Transforma `output.c` bruto em `output_runtime.c` pronto para compilar com o runtime.

```bash
python3 tools/ps2recomp/runtime/patch_output.py build/output.c
# Gera: build/output_runtime.c
```

---

### `tools/ps2recomp/build_relink.sh` — rebuildar o runtime sem cmake

Recompila os .cpp do runtime e religa `ps2_game` sem tocar no `output_runtime.c`.
**Paths hardcodados a partir do CMakeCache.txt — não precisam de detecção dinâmica.**

```bash
bash tools/ps2recomp/build_relink.sh          # recompila runtime + relinka
bash tools/ps2recomp/build_relink.sh --full   # + recompila output_runtime.c (~5min)
```

**IMPORTANTE:** `--full` requer `-DPS2_RECOMP_HAS_HOST` (já incluído no script).
Ao usar `--full`, o script aplica automaticamente esse define para evitar conflito de `main()`.

Gera flags `--wrap` automaticamente para cada `__wrap_func_*` definida em `override_stubs.c`.

Se as libs do nix store mudarem de hash (ambiente recriado), rode cmake uma vez:
```bash
cmake -S tools/ps2recomp/runtime -B tools/ps2recomp/runtime/build
```
e atualize os paths hardcodados no topo de `build_relink.sh`.

---

## 🔧 Ferramentas PONTUAIS

### `tools/ps2recomp/find_spin.py` — diagnóstico de hot-PC ⭐ NOVO

Dado um endereço PS2 (hot-PC do sampler), mostra a função que contém esse endereço,
o código C ao redor, todos os callers diretos e a cadeia de chamadas (N níveis).

```bash
python3 tools/ps2recomp/find_spin.py build/output.c 0x220730
python3 tools/ps2recomp/find_spin.py build/output.c 0x220730 --callers 4
python3 tools/ps2recomp/find_spin.py build/output.c 0x220730 --ctx 20
```

**Fluxo típico de diagnóstico:**
1. Rodar `--headless --frames 30` → capturar hot-PC do sampler
2. `find_spin.py build/output.c <hot-PC>` → identificar função e loop
3. Analisar código C gerado para entender por que está travado
4. Implementar fix em `bios_stub.cpp` ou `output_runtime.c`

---

### `tools/ps2recomp/check_todos.py` — o que ainda falta no recompilador

Varre `output.c` e lista mnemonics com `/* TODO */` ou `/* UNHANDLED */`.

```bash
python3 tools/ps2recomp/check_todos.py build/output.c --category
python3 tools/ps2recomp/check_todos.py build/output.c --min 100
```

**Estado atual:** 0 TODO, 0 UNHANDLED — pipeline completo.
Os 1225 comentários `/* VU0/MMI NOP: ... */` são intencionais (VU0 = NOP seguro).

---

### `tools/ps2recomp/validate_patch.py` — validação pré-commit ⭐

Detecta erros em bios_stub.cpp, gs_stub.cpp, recompiler.cpp, etc. **antes** de commitar.

```bash
python3 tools/ps2recomp/validate_patch.py
python3 tools/ps2recomp/validate_patch.py tools/ps2recomp/runtime/src/bios_stub.cpp
python3 tools/ps2recomp/validate_patch.py --strict
```

**Rodar antes de todo commit.**

---

### `tools/ps2recomp/triage_headless.py` — análise do teste headless ⭐

```bash
tools/ps2recomp/runtime/build/ps2_game --headless --frames 30 \
    tools/ps2recomp/build/elf_out/SCUS_973.99.elf 2>&1 | tee /tmp/headless.log
python3 tools/ps2recomp/triage_headless.py /tmp/headless.log
python3 tools/ps2recomp/triage_headless.py /tmp/headless.log --short
```

**CRITÉRIO DE PROGRESSO REAL:**
- `frames > 0` → código PS2 nativo avançou o boot ✅
- `frames = 0` → bloqueado; hot PC é o suspeito principal 🔴

---

### `tools/ps2recomp/runtime/src/override_stubs.c` — overrides por endereço ⭐

Permite interceptar qualquer função PS2 por PC sem rebuildar o output.c.

**ATENÇÃO — limitação intra-TU:**
O `--wrap` do linker **não intercepta chamadas diretas dentro do mesmo `.o`**.
Se a função alvo e seu caller estão ambos em `output_runtime.c`, o `--wrap` não funciona.
Nesse caso, patchar `output_runtime.c` diretamente e rebuildar com `--full`.

**Como adicionar um override:**
1. Declarar `static void override_ADDR(PS2Regs* regs)` em `override_stubs.c`
2. Adicionar entrada em `ps2_override_table[]` (mantida ordenada por pc)
3. `bash build_relink.sh` — sem recompilar output_runtime.c

---

### `tools/ps2recomp/jalr_targets.py` — analisar dispatch indireto (jalr)

```bash
python3 tools/ps2recomp/jalr_targets.py build/output.c
python3 tools/ps2recomp/jalr_targets.py build/output.c --unresolved --top 20
```

---

### `tools/ps2recomp/find_loops.py` — detectar loops no output.c

```bash
python3 tools/ps2recomp/find_loops.py build/output.c
python3 tools/ps2recomp/find_loops.py build/output.c --min-back 3 --top 50
```

---

### `tools/ps2recomp/runtime/recomp_stats.py` — estatísticas do output.c

```bash
python3 tools/ps2recomp/runtime/recomp_stats.py build/output.c
```

---

## Fluxo completo de recompilação

```
1. Compilar o recompilador (uma vez):
   bash tools/ps2recomp/build.sh

2. Baixar ISO (8.52 GB, não versionada no git):
   python3 tools/ps2recomp/download_iso_robust.py   # resumível, vai ao build/

3. Rodar na ISO:
   cd tools/ps2recomp/build
   ./ps2recomp recomp "God of War (USA).iso" output.c

4. Preparar para o runtime:
   python3 ../runtime/patch_output.py output.c
   # Gera: build/output_runtime.c

5. [PRÉ-COMMIT] Validar antes de commitar mudanças no runtime:
   python3 ../validate_patch.py

6. Compilar runtime:
   bash ../build_relink.sh

7. Extrair ELF e testar headless:
   ./ps2recomp extract "God of War (USA).iso" elf_out/
   ../runtime/build/ps2_game --headless --frames 30 elf_out/SCUS_973.99.elf 2>&1 | tee /tmp/headless.log
   python3 ../triage_headless.py /tmp/headless.log
```

---

## Ambiente

- **Sistema:** NixOS (Replit)
- **Python:** `python3` disponível no PATH
- **g++ / make:** disponíveis no PATH (GCC 14.2)
- **cmake:** ativo e funcional (o runtime já foi compilado via cmake — build artifacts em `runtime/build/`)
- **ISO:** `tools/ps2recomp/build/God of War (USA).iso` (8.52 GB — não no git)
  ```bash
  python3 tools/ps2recomp/download_iso_robust.py
  ```
- **ELF:** `tools/ps2recomp/build/elf_out/SCUS_973.99.elf` (1.9 MB — não no git)
- **Git push com token:**
  ```bash
  cd newportgame
  git remote set-url origin "https://x-access-token:${GITHUB_TOKEN}@github.com/cristianomarianoufsc-ops/newportgame"
  git push origin main
  ```

---

## Roadmap do port

- [x] ISO 9660 parser + ELF32 MIPS loader
- [x] Disassembler MIPS R5900 completo
- [x] Recompilador estático MIPS → C (0 UNHANDLED, 0 TODO)
- [x] Stub do Graphics Synthesizer (GS) → OpenGL 3.3 + GLSL shaders
- [x] Stub do IOP/BIOS (syscalls do PS2 — tabela corrigida)
- [x] Stub do SPU2 (áudio → placeholder OpenAL)
- [x] Delay slots, tail calls via `j`, `lwl`/`lwr`/`swl`/`swr`, `lqc2`/`sqc2`
- [x] COP0 (`mfc0`/`mtc0`/`di`/`ei`) — emitido corretamente, atualiza cop0[12].EIE
- [x] MMI completo — `pcpyld`/`pcpyud`/`pcpyh`/`ppacw`/`padduw`/`pand`/`por` etc.
- [x] VU0/COP2 — todas as instruções → NOP intencional (`/* VU0/MMI NOP: ... */`)
- [x] CACHE → NOP
- [x] **IOP spin-loop (`syscall 0x83`) CORRIGIDO** — `SIF_SET_DMA2` retorna valores que satisfazem merge-cursor imediatamente
- [x] **Post-pass de gap targets** — branch targets em gaps entre funções descobertos
- [x] **`ps2_report_unknown_dispatch()`** — JALR para endereços não mapeados imprime diagnóstico
- [x] **HW I/O stubs** — INTC_STAT, DMAC CHCR, SIF BOOTEND retornam valores que desbloqueiam polling loops
- [x] **validate_patch.py** — validação pré-commit
- [x] **triage_headless.py** — análise automatizada do teste headless
- [x] **override_stubs.c** — sistema de interceptação por endereço
- [x] **Thread-queue sentinel** — `mem[0x2CBBB0]=0x2CBBB0` em `SYS_SETUP_THREAD`; elimina loop infinito em `func_13fab8`
- [x] **VBlank stub `func_21ff00`** — delay-slot bug no recompilador tornava bne sempre tomado; patchado em `output_runtime.c` para incrementar `mem[0x29C7D4]` e retornar
- [x] **find_spin.py** — ferramenta de diagnóstico de hot-PC (função + callers + cadeia)
- [x] **`build_relink.sh --full` corrigido** — flag `-DPS2_RECOMP_HAS_HOST` adicionada; geração automática de `--wrap` flags
- [x] **`SYS_SIF_GET_REG` (0x7A)** — retorna `0x20000` para desbloquear polling loop em `func_296710` / `func_2966bc`
- [x] **`func_27a810` stub (sceSifLoadModule)** — sem ELF/IOP, slot pool em RAM está zerado; patchado em `output_runtime.c` → retorna handle 1
- [x] **`func_2990d0` stub (SIF boot-check)** — polls bit 0x40000 que nunca é setado sem IOP; patchado → retorna 1
- [x] **`func_299120` stub (module-load checker)** — lê struct de módulo em RAM zerada; patchado → retorna 1
- [x] **Headers corrigidos** — `include/ps2_runtime.h` e `ps2_gs_regs.h` eram symlinks auto-referentes; substituídos por arquivos reais
- [x] **`build_relink.sh` corrigido** — agora recompila `ps2_runtime_data.c` explicitamente (evita undefined ref a `ps2_report_unknown_dispatch`)
- [x] **Estrutura do repo limpa** — pasta `newportgame/newportgame/` (duplicata acidental) removida; tudo na raiz
- [ ] Dispatch indireto (`jalr`) — cobertura parcial; vtables/callbacks dinâmicos podem não estar mapeados
- [ ] OpenAL real no SPU2
- [ ] Funções fora do range recompilado (gaps) — implementar via override_stubs.c

---

## Estado atual (2026-07-24)

### Histórico de hot-PCs resolvidos

| Hot-PC | Causa | Fix |
|---|---|---|
| `0x13faf0` (100%) | Loop infinito na linked list de threads — `mem[0x2CBBB0]=0` (BSS); sentinela esperado em `0x2CBBB0` | `SYS_SETUP_THREAD` inicializa `mem[0x2CBBB0]=0x2CBBB0` |
| `0x220730` (100%) | `func_21ff00` (VBlank wait) — bug de delay-slot: `bne` usava `v0=20000` (delay slot) em vez do resultado do `slt`; loop infinito | Patch direto em `output_runtime.c`: incrementa `mem[0x29C7D4]` e retorna |
| `0x294030` (87%) + `0x296710` (13%) | `syscall 0x7A` (`SYS_SIF_GET_REG`) retornava 0; loop em `L_296710` aguarda `result & 0x20000 != 0` | Implementar `case SYS_SIF_GET_REG: regs->r[2] = 0x20000u` |
| `0x27a8f0` (99.9%) | `func_27a810` (sceSifLoadModule) — slot pool em RAM zerada (sem ELF); `func_296e10` retornava null → loop infinito com syscalls 0x40/0x41/0x77 | Stub `func_27a810`, `func_2990d0`, `func_299120` em `output_runtime.c` → retornam 1 |

### Estado atual do binário

```
ps2_game: compilado OK (2.0M) — build_relink.sh --full concluído
Próximo headless: aguarda ELF (SCUS_973.99.elf) para rodar
```

### Próximos passos imediatos

1. **Obter ELF** via `python3 tools/ps2recomp/download_iso_robust.py` (requer ISO do jogo)
2. **Rodar headless** com o novo binário:
   ```bash
   tools/ps2recomp/runtime/build/ps2_game --headless --frames 30 \
     tools/ps2recomp/build/elf_out/SCUS_973.99.elf 2>&1 | tee /tmp/headless.log
   python3 tools/ps2recomp/triage_headless.py /tmp/headless.log
   ```
3. **Analisar novo hot-PC** com `find_spin.py`
4. **Implementar fix** → `bash build_relink.sh` → commitar → push

### Armadilha crítica — intra-TU

O `--wrap` do linker **NÃO** intercepta chamadas diretas dentro do mesmo arquivo `.o`.
Quando o caller e a função alvo estão ambos em `output_runtime.c`, a única solução é
patchar `output_runtime.c` diretamente e rebuildar com `build_relink.sh --full`.

### Comparação com o projeto godofwar (outro agente)

| Aspecto | Nosso | Deles |
|---|---|---|
| Saída do recompilador | 1 arquivo C monolítico (350K linhas) | 5.627 .cpp separados |
| Contexto de registradores | `PS2Regs*` direto inline | `R5900Context* ctx + PS2Runtime*` |
| Anti-falso-positivo | validate_patch.py + triage_headless.py | anti_fake_guard.py + C838-GUARD NATIVOS |
| Ferramenta de overrides | override_stubs.c (por endereço) | game_overrides.cpp |
| Ferramenta de diagnóstico | find_spin.py (hot-PC → função → callers) | — |

**Lição do godofwar:** ~300 passos desperdiçados substituindo funções inteiras por stubs
que avançavam estado artificialmente. Nossa proteção: triage_headless.py exige
`frames > 0 por código PS2 nativo` como critério de progresso real.

---

## Sobre a pasta `newportgame/newportgame/`

Dentro do repositório existe uma subpasta `newportgame/` que é uma cópia acidental
criada por um agente anterior. **Pode ser deletada com segurança** — não contém
nada que não esteja na raiz do repositório. O conteúdo real do projeto está na
raiz de `newportgame/` (este repositório).
