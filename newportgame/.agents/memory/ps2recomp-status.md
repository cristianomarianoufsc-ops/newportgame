---
name: PS2 Recompiler — God of War 1 status
description: Estado completo do projeto: o que foi implementado, TODOs restantes, pipeline de rebuild
---

## Números atuais
```
TODO total      :    875   (era 2.849 no início — 69% de redução)
UNHANDLED total :  1.535   (era 3.215 — 52% de redução)
Total issues    :  2.410   (era 6.064 — 60% de redução acumulada)
mnemonics únicos:     33
```

---

## O que foi implementado (acumulado neste agente)

### disasm.cpp
- `sll/srl/sra/sllv/srlv/srav/dsllv/dsrlv/dsrav` → `InstrCategory::SHIFT`
- `movz/movn` → `InstrCategory::MOVE`
- Tabela `op_names[64]` COP1 completa:
  - PS2-específicos: `adda(0x18)`, `suba(0x19)`, `mula(0x1a)`, `madd(0x1c)`, `msub(0x1d)`, `madda(0x1e)`, `msuba(0x1f)`, `max(0x28)`, `min(0x29)`, `cvt.s(0x20)`, `cvt.d(0x21)`
  - MIPS compare: `c.f..c.ngt` (0x30–0x3f)

### recompiler.cpp — case InstrCategory::FLOAT (novo)
- `mtc1/mfc1/ctc1/cfc1` (GPR↔FPU bit-exact via memcpy)
- `mov.s`, `add.s`, `sub.s`, `mul.s`, `div.s` (div-by-zero → ±MAX_FLOAT como HW)
- `sqrt.s`, `abs.s`, `neg.s`
- `max.s`, `min.s` (PS2-específicos, sem fmaxf para preservar semântica EE)
- `adda.s/suba.s/mula.s` → escrevem em `regs->f[32]` (ACC)
- `madd.s/msub.s` → `fd = ACC ± fs*ft`; `madda.s/msuba.s` → `ACC ±= fs*ft`
- `cvt.s.s` com detecção de fmt: fmt=0x14 (W) → int32→float; fmt=0x10 (S) → NOP
- `cvt.w.s`, `round.w.s`, `trunc.w.s`, `ceil.w.s`, `floor.w.s`
- Compare: `c.eq/c.lt/c.le/c.olt/c.ole/c.ult/c.ule/c.f/c.sf/c.un` + variantes → fcr31 bit 23
- `bc1t/bc1f` no case BRANCH

### recompiler.cpp — outros cases
- `jalr` → `ps2_dispatch()`
- `ld/sd` 64-bit, `lq/sq` 128-bit, `ldl/ldr/sdl/sdr`, `lwc1/swc1`, `lwu`
- ALU 64-bit: `daddi/daddiu/daddu/dadd/dsub/dsubu`
- Shifts 64-bit: `dsll/dsrl/dsra/dsll32/dsrl32/dsra32/dsllv/dsrlv/dsrav`
- `movz/movn`, `bgezal/bltzal`
- Cross-function branch stubs via `ps2_dispatch`
- 32-bit shifts sign-extendem para 64-bit (EE-correto)

### ps2_runtime.h
- `mem_read64/mem_write64` inline
- `#include <math.h>`
- `float f[33]` — f[0-31]=FP registers, **f[32]=ACC** (acumulador PS2)

### Ferramentas Python
- `check_todos.py --category` — analisa output.c por categoria
- `jalr_targets.py` — rastreia call sites de jalr
- `patch_output.py` — prepara output.c para o runtime

---

## ⚠️ Armadilha crítica: sincronização disasm.cpp

**Sempre usar `touch` antes de `make`** — se o cp ocorrer no mesmo segundo que o build anterior, o make não recompila:

```bash
cp newportgame/tools/.../disasm.cpp     tools/.../disasm.cpp
cp newportgame/tools/.../recompiler.cpp tools/.../recompiler.cpp
touch tools/ps2recomp/src/mips/disasm.cpp \
      tools/ps2recomp/src/recomp/recompiler.cpp
cd tools/ps2recomp/build && make -j$(nproc)
# Verificar: deve mostrar "Building CXX object ... disasm.cpp.o"
```

---

## TODO restante por prioridade

| Categoria | Itens | Count |
|---|---|---|
| `???` UNHANDLED | opcodes EE desconhecidos | 1.105 |
| `sqc2` TODO | COP2/VU0 store 128-bit | 466 |
| `lqc2` TODO | COP2/VU0 load 128-bit | 310 |
| `pcpyld/pcpyh/pcpyud` UNHANDLED | MMI pack/copy 64-bit | 158 |
| `break` UNHANDLED | debug breakpoint (pode stub vazio) | 76 |
| `ppacw/padduw/pand/por/pnor` UNHANDLED | MMI SIMD | 146 |
| `lwl/lwr` TODO | load word left/right (unaligned) | 34 |
| `swl/swr` TODO | store word left/right | 22 |
| `special?` TODO | opcodes SPECIAL não reconhecidos | 43 |

> **Próximos passos sugeridos (em ordem):**
> 1. `lwl/lwr/swl/swr` — 56 itens, simples de implementar (byte-lane unaligned)
> 2. `break` — stub vazio (1 linha)
> 3. MMI básico: `pcpyld/pcpyud/pcpyh` — pack/copy de 64-bit entre registradores de 128-bit
> 4. `lqc2/sqc2` — COP2 stubs (pelo menos não travar)
> 5. Compilar runtime no host MX Linux com SDL2

---

## Pipeline de rebuild completo
```bash
# 1. Editar em newportgame/tools/ps2recomp/src/
# 2. Sincronizar
cp newportgame/tools/ps2recomp/src/mips/disasm.cpp     tools/ps2recomp/src/mips/disasm.cpp
cp newportgame/tools/ps2recomp/src/recomp/recompiler.cpp tools/ps2recomp/src/recomp/recompiler.cpp
cp newportgame/tools/ps2recomp/runtime/include/ps2_runtime.h tools/ps2recomp/runtime/include/ps2_runtime.h
touch tools/ps2recomp/src/mips/disasm.cpp tools/ps2recomp/src/recomp/recompiler.cpp
# 3. Rebuild
cd tools/ps2recomp/build && make -j$(nproc)
# 4. Gerar e verificar
./ps2recomp recomp "God of War (USA).iso" output.c
python3 .../check_todos.py output.c --category
python3 .../runtime/patch_output.py output.c output_runtime.c
# 5. Copiar de volta para git
cp tools/ps2recomp/build/{ps2recomp,output.c,output_runtime.c} \
   newportgame/tools/ps2recomp/build/
# 6. Commit + push
cd newportgame && git add -f tools/ps2recomp/build/{ps2recomp,output.c,output_runtime.c}
git add tools/ps2recomp/src/ tools/ps2recomp/runtime/ .agents/memory/ AGENTS.md
git commit -m "..." && git push origin main
```

## Arquivos-chave
| Arquivo | Descrição |
|---|---|
| `tools/ps2recomp/src/recomp/recompiler.cpp` | emit_instruction — toda a lógica de geração de C |
| `tools/ps2recomp/src/mips/disasm.cpp` | Decodificador MIPS — InstrCategory e op_names COP1 |
| `tools/ps2recomp/runtime/include/ps2_runtime.h` | PS2Regs (f[33]), mem helpers, math.h |
| `tools/ps2recomp/runtime/CMakeLists.txt` | Build do runtime (-DPS2_RECOMP_HAS_HOST) |
| `tools/ps2recomp/build/output.c` | C gerado — 1426 funções |
