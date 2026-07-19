---
name: PS2 Recompiler — God of War 1 status
description: Estado do projeto de recompilação estática PS2→PC, o que foi implementado e próximos passos
---

## Estado atual (atualizado)

### Recompilador — o que foi implementado nesta sessão

**recompiler.cpp:**
- `jalr` — dispatch indireto via `ps2_dispatch()` (tabela switch gerada no output.c)
- `ld` / `sd` — load/store 64-bit via `mem_read64` / `mem_write64`
- `lwc1` / `swc1` — load/store FPU via memcpy
- `lq` / `sq` — load/store 128-bit (lower 64 bits por ora)
- `ldl` / `ldr` / `sdl` / `sdr` — approx via 64-bit
- `lwu` — load word unsigned zero-extended
- `daddi` / `daddiu` / `daddu` / `dadd` / `dsubu` / `dsub` — ALU 64-bit
- `dsll` / `dsrl` / `dsra` / `dsll32` / `dsrl32` / `dsra32` — shifts 64-bit imediato
- `dsllv` / `dsrlv` / `dsrav` — shifts 64-bit variável
- `movz` / `movn` — moves condicionais
- 32-bit shifts agora sign-extendem para 64 bits (EE-correto)
- Cross-function branches: emite stub `L_XXXX: ps2_dispatch(0xXXXXu, regs); return;`
- `emit_pc_tracking` agora usa helper `HEX()` para não contaminar stream com `std::hex`
- Adicionado lambda `SA(shamt)` → sempre decimal (evita bug hex no shamt)

**disasm.cpp:**
- `sll`, `srl`, `sra` → `InstrCategory::SHIFT` (antes herdavam ALU por default)
- `sllv`, `srlv`, `srav` → `InstrCategory::SHIFT`
- `dsllv`, `dsrlv`, `dsrav` → `InstrCategory::SHIFT`
- `movz`, `movn` → `InstrCategory::MOVE`

**ps2_runtime.h:**
- `mem_read64` / `mem_write64` inline (via memcpy para portabilidade)

**runtime/CMakeLists.txt:**
- `-DPS2_RECOMP_HAS_HOST` adicionado para evitar duplicata de `ps2_game_start`

**runtime/patch_output.py:**
- Lida com ps2_game_start já existente (recompilador novo); remove main() redundante

### Resultado do check_todos após todas as correções
```
TODO total      :      875   (era 2.849 antes — 69% reduzido)
UNHANDLED total :    3.215
```

### TODO restante por prioridade

| Categoria | Total | Itens principais |
|---|---|---|
| FPU move | 859 | `mtc1` (494), `mov.s` (205), `mfc1` (160) |
| FPU arith | 536 | `mul.s` (216), `sub.s` (103), `add.s` (89), `div.s` (84), `neg.s` (39) |
| VU0 store | 466 | `sqc2` (466) — COP2 store |
| VU0 load | 310 | `lqc2` (310) — COP2 load |
| other | ~1684 | `???` (1105 — opcode desconhecido), `cop1?` (231), `ppacw` (59) |
| misc | 76 | `break` (76) |

> **Próximo passo prioritário: FPU (COP1)**
> `mtc1`, `mfc1`, `mov.s`, `mul.s`, `add.s`, `sub.s`, `div.s`, `neg.s`, `abs.s`
> Implementar em `emit_instruction()` no case `InstrCategory::FLOAT` (ou onde o disasm colocar).
> ATENÇÃO: verificar em disasm.cpp qual InstrCategory as instruções COP1 recebem antes de implementar.

### Ferramentas Python disponíveis
| Arquivo | Uso |
|---|---|
| `tools/ps2recomp/check_todos.py --category` | Lista TODOs por categoria após novo output.c |
| `tools/ps2recomp/jalr_targets.py` | Analisa call sites de jalr |
| `tools/ps2recomp/find_loops.py` | Detecta loops (backward gotos) |
| `tools/ps2recomp/runtime/patch_output.py` | Prepara output.c para o runtime |

### Pipeline de rebuild (ordem exata)
```bash
cd tools/ps2recomp/build
make -j$(nproc)                                              # recompila ps2recomp
./ps2recomp recomp "God of War (USA).iso" output.c          # gera output.c
python3 ../check_todos.py output.c --category               # verifica TODOs restantes
python3 ../runtime/patch_output.py output.c output_runtime.c
cd ../runtime/build && make -j$(nproc)                      # compila ps2_game
./ps2_game --headless --frames 50 ../../../elf_out/SCUS_973.99.elf
```

### Arquivos-chave
| Arquivo | Descrição |
|---|---|
| `tools/ps2recomp/src/recomp/recompiler.cpp` | emit_instruction, emit_function, emit_c |
| `tools/ps2recomp/src/mips/disasm.cpp` | Decodificador MIPS — InstrCategory aqui |
| `tools/ps2recomp/runtime/include/ps2_runtime.h` | mem_read64/write64, PS2Regs |
| `tools/ps2recomp/runtime/CMakeLists.txt` | Build do runtime (tem -DPS2_RECOMP_HAS_HOST) |
| `tools/ps2recomp/runtime/src/host_main.cpp` | Main SDL2 + headless mode |

**Why:** `jalr`+`ld`/`sd`+shifts eram a causa raiz do loop infinito. FPU é o próximo gargalo.
**How to apply:** Implementar COP1 — verificar InstrCategory no disasm, depois implementar em emit_instruction.
