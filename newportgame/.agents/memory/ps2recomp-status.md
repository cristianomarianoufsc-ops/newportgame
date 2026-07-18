---
name: PS2 Recompiler — God of War 1 status
description: Estado do projeto de recompilação estática PS2→PC, próximos passos e diagnóstico do loop infinito
---

## Estado atual (commitado e pushado)

### O que funciona
- `tools/ps2recomp/build/ps2recomp` — binário do recompilador compilado limpo
- `tools/ps2recomp/build/elf_out/SCUS_973.99.elf` — ELF extraído da ISO (1.9 MB)
- `tools/ps2recomp/build/output_runtime.c` — 1426 funções + 74 stubs (~11.9 MB de C)
- `tools/ps2recomp/runtime/build/ps2_game` — binário PC (1.3 MB), compila limpo com SDL2+GLEW+OpenAL
- Modo `--headless --frames N` funciona (sem SDL/GL)
- Handlers de sinal (SIGSEGV, SIGBUS, SIGALRM) com dump de stats

### Diagnóstico do loop infinito
Rodar `./ps2_game --headless --frames 10 SCUS_973.99.elf` → SIGALRM após 25s, **0 syscalls, 0 frames, 0 GS writes**.

**Causa raiz:** o código gerado está preso num loop antes de qualquer SYSCALL.
Possíveis causas (a investigar):
1. **Indirect call (`jalr $v0`) emitido como `/* indirect call via regs->r[2] — TODO dynamic dispatch */`** — a linha 2650 do output.c mostra `jalr $v0` virou comentário. A função chamada nunca executa, o loop não avança.
2. Polling de registrador de hardware (INTC, DMAC) que retorna 0 e nunca libera.
3. `ld`/`sd`/`lwc1`/`swc1` são `/* TODO */` — stack frame corrompido, $ra errado, return errado.

**Próximo passo imediato:**
- Implementar `jalr` no recompilador (`emit_instruction` para `jalr`) como dispatch via tabela de funções por endereço.
- Implementar `ld`/`sd` (load/store doubleword 64-bit) — críticos para salvar/restaurar $ra.
- Implementar `lwc1`/`swc1` (load/store FPU) — usados extensivamente.

### Localização dos arquivos
| Arquivo | Descrição |
|---|---|
| `tools/ps2recomp/src/recomp/recompiler.cpp` | Recompilador — emit_instruction, emit_function, emit_c |
| `tools/ps2recomp/src/mips/disasm.cpp` | Decodificador MIPS |
| `tools/ps2recomp/runtime/src/host_main.cpp` | Main SDL2 + headless mode |
| `tools/ps2recomp/runtime/src/gs_stub.cpp` | GS → OpenGL 3.3 |
| `tools/ps2recomp/runtime/src/bios_stub.cpp` | Syscall stubs + freq counter |
| `tools/ps2recomp/runtime/include/ps2_runtime.h` | Header compartilhado |
| `God of War (USA).iso` | ISO na raiz do workspace (8.52 GB) |
| `tools/ps2recomp/build/elf_out/SCUS_973.99.elf` | ELF extraído |

### Pipeline de rebuild
```bash
bash tools/ps2recomp/build.sh
cd tools/ps2recomp/build && ./ps2recomp recomp "God of War (USA).iso" output.c
python3 ../runtime/patch_output.py output.c output_runtime.c
cd ../runtime/build && make -j$(nproc)
./ps2_game --headless --frames 10 ../elf_out/SCUS_973.99.elf
```

**Why:** Sem jalr e sem ld/sd o jogo não passa da inicialização.
**How to apply:** Próxima sessão deve começar por implementar jalr + ld/sd no recompilador.
