# AGENTS.md — Leitura obrigatória para agentes

Este documento descreve as ferramentas disponíveis neste projeto e como usá-las.
**Todo agente que abrir este repositório deve ler este arquivo antes de trabalhar.**

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

Se as libs do nix store mudarem de hash (ambiente recriado), rode cmake uma vez:
```bash
cmake -S tools/ps2recomp/runtime -B tools/ps2recomp/runtime/build
```
e atualize os paths hardcodados no topo de `build_relink.sh`.

---

## 🔧 Ferramentas PONTUAIS

### `tools/ps2recomp/check_todos.py` — o que ainda falta no recompilador

Varre `output.c` e lista mnemonics com `/* TODO */` ou `/* UNHANDLED */`.

```bash
python3 tools/ps2recomp/check_todos.py build/output.c --category
python3 tools/ps2recomp/check_todos.py build/output.c --min 100
```

**Estado atual:** 0 TODO, 0 UNHANDLED — pipeline completo.
Os 1225 comentários `/* VU0/MMI NOP: ... */` são intencionais (VU0 = NOP seguro).

---

### `tools/ps2recomp/validate_patch.py` — validação pré-commit ⭐ NOVO

Detecta erros em bios_stub.cpp, gs_stub.cpp, recompiler.cpp, etc. **antes** de commitar.
Suporta raw strings `R"..."`, line-continuation e comentários /* */.

```bash
# Verifica todos os arquivos do runtime
python3 tools/ps2recomp/validate_patch.py

# Arquivo específico
python3 tools/ps2recomp/validate_patch.py tools/ps2recomp/runtime/src/bios_stub.cpp

# Modo estrito (exit 2 se houver TODOs)
python3 tools/ps2recomp/validate_patch.py --strict
```

**Rodar antes de todo commit** — evita round desperdiçado por erro de compilação.

---

### `tools/ps2recomp/triage_headless.py` — análise do teste headless ⭐ NOVO

Analisa a saída capturada de `ps2_game --headless` e entrega relatório estruturado:
- **PROGRESSO REAL** (frames > 0 por código PS2 nativo) vs **BLOQUEADO** vs **CRASH**
- Top-15 hot PCs do sampler (spin loops)
- Syscalls mais chamadas
- Diagnóstico do bloqueador

```bash
# Rodar o jogo headless e capturar log
tools/ps2recomp/runtime/build/ps2_game --headless --frames 30 \
    tools/ps2recomp/build/elf_out/SCUS_973.99.elf 2>&1 | tee /tmp/headless.log

# Analisar
python3 tools/ps2recomp/triage_headless.py /tmp/headless.log

# Modo curto (só status + top PC)
python3 tools/ps2recomp/triage_headless.py /tmp/headless.log --short

# Comparar com run anterior (delta de frames/gs_writes)
python3 tools/ps2recomp/triage_headless.py /tmp/headless.log --compare /tmp/headless_prev.log
```

**CRITÉRIO DE PROGRESSO REAL** (análogo ao C838-GUARD NATIVOS do godofwar):
- `frames_completados > 0` → código PS2 nativo avançou o boot ✅
- `frames_completados = 0` → bloqueado; hot PC é o suspeito principal 🔴

---

### `tools/ps2recomp/runtime/src/override_stubs.c` — overrides por endereço ⭐ NOVO

Permite interceptar qualquer função PS2 por PC **sem rebuildar o output.c**.
Analogia ao `game_overrides.cpp` do projeto godofwar.

**Como adicionar um override:**
1. Declarar `static void override_ADDR(PS2Regs* regs)` em `override_stubs.c`
2. Adicionar entrada em `ps2_override_table[]` (mantida ordenada por pc)
3. Rodar `bash build_relink.sh` — sem recompilar output_runtime.c

**Regra anti-falso-positivo:**
- Override que force-retorna valor sem código PS2 real = mascaramento, não progresso
- Só usar para funções **fora do range recompilado** (gaps) ou com análise do binário

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
- **Python:** `/nix/store/010r29jy64nj14dx7fabacypr4f2q077-python3-3.11.9-env/bin/python3`
  `python3` pode NÃO estar no PATH — use o path acima explicitamente se necessário.
  ```bash
  PYTHON=/nix/store/010r29jy64nj14dx7fabacypr4f2q077-python3-3.11.9-env/bin/python3
  $PYTHON tools/ps2recomp/check_todos.py build/output.c --category
  ```
- **g++ / make:** disponíveis no PATH (GCC 14.2)
- **cmake:** ativo e funcional (o runtime já foi compilado via cmake — build artifacts em `runtime/build/`)
- **ISO:** `tools/ps2recomp/build/God of War (USA).iso` (8.52 GB — não no git)
  ```bash
  # Download resumível (usa urllib, sem pip):
  python3 tools/ps2recomp/download_iso_robust.py
  ```
- **SDL2 headers (nix):** `/nix/store/6vl9b59i822mh3zmri5g4kywahzhp5zw-sdl2-compat-2.32.56-dev/include`
  (hardcodado em `build_relink.sh` — se mudar, atualizar lá)
- **Git push:**
  ```bash
  cd tools/ps2recomp && git push https://$GITHUB_TOKEN@github.com/cristianomarianoufsc-ops/newportgame HEAD:main
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
- [x] **IOP spin-loop (`syscall 0x83`) CORRIGIDO** — `SIF_SET_DMA2` retorna valores alternados que satisfazem merge-cursor imediatamente
- [x] **Post-pass de gap targets** — branch targets em gaps entre funções descobertos e adicionados ao BFS
- [x] **`ps2_report_unknown_dispatch()`** — JALR para endereços não mapeados imprime diagnóstico (rate-limited)
- [x] **HW I/O stubs** — INTC_STAT, DMAC CHCR, SIF BOOTEND retornam valores que desbloqueiam polling loops
- [x] **validate_patch.py** — validação pré-commit (chaves, parênteses, strings, TODOs)
- [x] **triage_headless.py** — análise automatizada do teste headless (PROGRESSO REAL vs BLOQUEADO)
- [x] **override_stubs.c** — sistema de interceptação por endereço sem recompilar output.c
- [ ] **Teste pós-fix 0x83** — testar `--headless --frames 30` com ELF real (aguardando ISO)
- [ ] Dispatch indireto (`jalr`) — cobertura parcial; vtables/callbacks dinâmicos podem não estar mapeados
- [ ] OpenAL real no SPU2
- [ ] Funções fora do range recompilado (gaps) — implementar via override_stubs.c

---

## Estado atual e próximos passos

### Problemas conhecidos

| Problema | Severidade | Detalhe |
|---|---|---|
| **Teste pós-fix 0x83** | 🔴 Crítico | ISO não baixada ainda. Quando disponível: extrair ELF → `--headless --frames 30` → `triage_headless.py` |
| **Build SDL2 headers** | ✅ Resolvido | Paths hardcodados no `build_relink.sh` a partir do CMakeCache |
| **`jalr` cobertura parcial** | 🟠 Médio | `ps2_dispatch()` cobre funções via `jal`/`j` estático; vtables/callbacks podem falhar |
| **SPU2 sem áudio real** | 🟡 Baixo | Stub absorve writes sem produzir som |

### Sequência pós-ISO

```bash
# 1. Verificar que a ISO está completa
ls -lh tools/ps2recomp/build/"God of War (USA).iso"   # esperado: ~8.52 GB

# 2. Extrair ELF
cd tools/ps2recomp/build
./ps2recomp extract "God of War (USA).iso" elf_out/

# 3. Rebuildar runtime (se bios_stub.cpp foi alterado)
bash ../build_relink.sh

# 4. Testar headless
../runtime/build/ps2_game --headless --frames 30 elf_out/SCUS_973.99.elf \
    2>&1 | tee /tmp/headless.log

# 5. Analisar resultado
PYTHON=/nix/store/010r29jy64nj14dx7fabacypr4f2q077-python3-3.11.9-env/bin/python3
$PYTHON ../triage_headless.py /tmp/headless.log

# 6. Se frames=0 → ver hot PC no sampler → identificar syscall/função bloqueante
#    → implementar em bios_stub.cpp ou via override_stubs.c
#    → rodar validate_patch.py antes de commitar
#    → rebuild_relink.sh → testar novamente
```

### Diagnóstico ANTERIOR ao fix do 0x83

```
syscall 0x83 : 2.576.953.245 (loop infinito no merge de cursores DMA)
frames=0  gs_writes=0
```

**Fix aplicado** (`bios_stub.cpp`): retorna valores alternados `BASE+524` / `BASE+360`
(`BASE=0x500000`) — `s3-524 == s2-360 = BASE` → loop sai imediatamente.

### Comparação com o projeto godofwar (outro agente)

| Aspecto | Nosso | Deles |
|---|---|---|
| Saída do recompilador | 1 arquivo C monolítico (350K linhas) | 5.627 .cpp separados |
| Contexto de registradores | `PS2Regs*` direto inline | `R5900Context* ctx + PS2Runtime*` |
| Estado atual | 0 UNHANDLED/TODO, aguardando ISO | PASSO 564+, bloqueado em state machine c838 |
| Anti-falso-positivo | validate_patch.py + triage_headless.py | anti_fake_guard.py + C838-GUARD NATIVOS |
| Ferramenta de overrides | override_stubs.c (por endereço) | game_overrides.cpp (5174 linhas) |
| Build inicial | minutos (1 .c) | horas (5627 .cpp) |

**Lição do godofwar:** ~300 passos desperdiçados porque substituíam funções inteiras por stubs
que avançavam estado artificialmente. A causa raiz (função `0x35C1B0` fora do range
recompilado que escrevia o próximo handler) nunca foi resolvida.
**Nossa proteção:** override_stubs.c com regra anti-falso-positivo explícita + triage_headless.py
que exige `frames > 0 por código PS2 nativo` como critério de progresso real.
