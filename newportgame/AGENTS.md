# AGENTS.md — Leitura obrigatória para agentes

Este documento descreve as ferramentas disponíveis neste projeto e como usá-las.
**Todo agente que abrir este repositório deve ler este arquivo antes de trabalhar.**

---

## Objetivo do projeto

Port nativo de PS2 para PC via recompilação estática.
Fluxo principal: ISO de PS2 → executável MIPS extraído → traduzido para C → compilado para x86-64.

---

## ⚙️ Ferramentas OBRIGATÓRIAS

Sem estas ferramentas o fluxo principal não funciona. Sempre verifique que estão disponíveis antes de qualquer outra coisa.

### `tools/ps2recomp/build.sh` — compilar o recompilador

Compila o binário `ps2recomp` que processa a ISO e gera o `output.c`.

```bash
bash tools/ps2recomp/build.sh
# Gera: tools/ps2recomp/build/ps2recomp
```

**Dependências no ambiente NixOS (Replit):**
- `g++` e `make` já estão no PATH
- `cmake` está **quebrado** no Replit (segfault) — o `build.sh` usa `g++` diretamente, sem cmake

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

Transforma o `output.c` bruto do recompilador em `output_runtime.c`, pronto para compilar com o runtime nativo.
Sem este passo o output.c **não compila** junto com o runtime.

O que faz:
- Substitui o header inline por `#include "ps2_runtime.h"`
- Renomeia `int main(void)` → `void ps2_game_start(void)`

```bash
python3 tools/ps2recomp/runtime/patch_output.py build/output.c
# Gera: build/output_runtime.c
```

---

## 🔧 Ferramentas PONTUAIS

Úteis para análise e diagnóstico, mas não bloqueiam o fluxo principal. Use quando precisar de informação específica.

### `tools/ps2recomp/check_todos.py` — o que ainda falta implementar no recompilador

Varre o `output.c` e lista todos os mnemonics que ainda geram `/* TODO */` ou `/* UNHANDLED */`, com contagem e categoria. **Use isso logo após gerar um novo output.c para saber o que priorizar a seguir.**

```bash
# Visão geral com categorias
python3 tools/ps2recomp/check_todos.py build/output.c --category

# Só o que aparece >= 100 vezes
python3 tools/ps2recomp/check_todos.py build/output.c --min 100
```

---

### `tools/ps2recomp/jalr_targets.py` — analisar padrões de dispatch indireto (jalr)

Analisa todos os call sites de `jalr` no `output.c`: tenta resolver o registrador fonte por data-flow simples, detecta funções atingidas só via jalr e separa os call sites dinâmicos (não resolvíveis) dos estáticos. **Use quando o jogo travar em dispatch indireto.**

```bash
# Visão geral — top 30 funções com mais jalr
python3 tools/ps2recomp/jalr_targets.py build/output.c

# Focar nos call sites não resolvíveis (jalr via registrador dinâmico)
python3 tools/ps2recomp/jalr_targets.py build/output.c --unresolved --top 20
```

---

### `tools/ps2recomp/find_loops.py` — detectar loops no output.c

Identifica funções com backward gotos (loops). Útil para priorizar quais funções precisam de tratamento especial no port.

```bash
# Top 30 funções com mais loops
python3 tools/ps2recomp/find_loops.py build/output.c

# Detalhe de uma função específica
python3 tools/ps2recomp/find_loops.py build/output.c --func func_001f0000

# Filtrar por mínimo de loops
python3 tools/ps2recomp/find_loops.py build/output.c --min-back 3 --top 50
```

**Quando usar:** ao iniciar trabalho em uma nova função ou antes de decidir por qual função começar o port.

---

### `tools/ps2recomp/runtime/recomp_stats.py` — estatísticas do output.c

Visão geral do que foi gerado: total de funções, instruções por categoria, TODOs pendentes, maiores funções.

```bash
python3 tools/ps2recomp/runtime/recomp_stats.py build/output.c
```

**Quando usar:** após gerar um novo `output.c` para entender o tamanho e complexidade do trabalho à frente.

---

## Fluxo completo de recompilação

```
1. Compilar o recompilador (obrigatório, uma vez):
   bash tools/ps2recomp/build.sh

2. Rodar na ISO (obrigatório):
   cd tools/ps2recomp/build
   ./ps2recomp recomp "God of War (USA).iso" output.c

3. [PONTUAL] Ver estatísticas do que foi gerado:
   python3 ../runtime/recomp_stats.py output.c

4. [PONTUAL] Identificar funções com loops:
   python3 ../find_loops.py output.c

5. Preparar para o runtime (obrigatório):
   python3 ../runtime/patch_output.py output.c
   # Gera output_runtime.c

6. Compilar runtime + output_runtime.c:
   cd ../runtime
   bash build_runtime.sh

7. Extrair ELF da ISO e rodar:
   cd ../build
   ./ps2recomp extract "God of War (USA).iso" out/
   ../runtime/build/ps2_game out/SCUS_973.99

   # Ou headless (sem janela):
   ../runtime/build/ps2_game --headless --frames 10 out/SCUS_973.99
```

---

## Ambiente

- **Sistema:** NixOS (Replit) — não usar `apt`. Usar `nix-env` ou o skill de pacotes do Replit.
- **Python:** 3.12 instalado (`python3` disponível no PATH) — nenhum pacote externo necessário
- **g++ / make:** disponíveis no PATH (GCC 14.3)
- **cmake:** **NÃO USAR** — segfault no Replit. Build usa g++ direto (ver acima)
- **ISO:** `tools/ps2recomp/build/God of War (USA).iso` (8.52 GB — não versionada no git)
  ```bash
  pip install gdown -q && gdown "1ruRDjG5J0FrCVSU1WdNQqehIoT7csS0S" -O tools/ps2recomp/build/
  ```

---

## Roadmap do port

- [x] ISO 9660 parser
- [x] ELF32 MIPS loader
- [x] Disassembler MIPS R5900
- [x] Recompilador estático MIPS → C
- [x] Stub do Graphics Synthesizer (GS) → OpenGL 3.3
- [x] Stub do IOP/BIOS (syscalls do PS2)
- [x] Stub do SPU2 (áudio → placeholder OpenAL)
- [x] **Delay slots corrigidos** — delay slot emitido ANTES do branch/jump; likely branches tratadas separadamente
- [x] **Tail calls via `j`** — `j` fora dos bounds da função agora vira tail call e é adicionado ao BFS de descoberta
- [x] **`lwl`/`lwr`/`swl`/`swr`** — unaligned memory access implementado (little-endian PS2 EE)
- [x] **COP0 (`eret`, `ei`, `di`)** — decodificado como NOP no host
- [x] **`sync`** — SPECIAL funct=0x0F decodificado como NOP
- [x] **`break`** — TRAP no-op em vez de UNHANDLED
- [x] **Label em delay-slot** — labels emitidos ANTES do consumed_pcs check para evitar `goto` sem definição
- [x] **1449 funções descobertas** — (era 1426) — BFS encontra funções via `j` tail call
- [x] **Pipeline completo funcionando** — 2.5B syscalls executados headless (era 0)
- [ ] **IOP spin-loop** — syscall 0x83 chamado em loop; IOP stub precisa simular evento/sinal
- [ ] Dispatch indireto (`jalr` — chamadas por ponteiro) — `ps2_dispatch()` cobre apenas funções descobertas via `jal`/`j` estático
- [ ] Instruções MMI e VU0 (operações vetoriais) — parcialmente no disassembler, não traduzidas no recompilador
- [ ] OpenAL real no SPU2 (atualmente absorve writes sem produzir som)
- [ ] Descoberta de funções via `jalr` (vtables, callbacks não alcançados por jal/j estático)

---

## Problemas conhecidos / TODOs críticos

| Problema | Severidade | Detalhe |
|---|---|---|
| **IOP spin-loop (`syscall 0x83`)** | 🔴 Crítico | O jogo executa syscall 0x83 bilhões de vezes esperando resposta do IOP; frames=0. Implementar sleep/wakeup real no bios_stub ou simular evento IOP |
| **MMI / VU0 não traduzidos** | 🟠 Médio | `pcpyld`(128), `ppacw`(59), `padduw`(29), `pand`(24) etc. — UNHANDLED. Impacta operações vetoriais |
| **`jalr` cobertura parcial** | 🟠 Médio | `ps2_dispatch()` cobre apenas funções via `jal`/`j` estático; vtables/callbacks dinâmicos não alcançados |
| **SPU2 sem áudio real** | 🟡 Baixo | Stub absorve writes mas não produz som; OpenAL não conectado |

### Diagnóstico atual (headless --frames 30)

```
syscall 0x83 : 2.576.953.245 (IOP yield loop)
syscall 0x74 : 2
syscall 0x40 : 2  (CreateSema)
syscall 0x3d : 1
syscall 0x3c : 1
frames=0  gs_writes=0
```

O `syscall 0x83` é provavelmente `sceSifSendCmd` ou `sceSifDmaStat` — o jogo aguarda o IOP (Sound Processor / IO Processor) inicializar, mas o stub não sinaliza conclusão. **Próximo passo**: identificar syscall 0x83 no SDK do PS2 e fazer o bios_stub retornar o valor correto para liberar o loop.
