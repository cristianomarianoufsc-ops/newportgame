# PS2 Runtime — GS → OpenGL

Runtime que permite executar o código recompilado (`output.c`) no PC.

## Arquitetura

```
output_runtime.c    ← output.c patchado (ps2_game_start em vez de main)
ps2_runtime_data.c  ← define ps2_ram[32MB], ps2_spr[16KB]
gs_stub.cpp         ← GS registers → OpenGL 3.3 draw calls
bios_stub.cpp       ← EE BIOS syscalls → stubs host-side
spu2_stub.cpp       ← SPU2 audio → (placeholder, → OpenAL futuro)
host_main.cpp       ← SDL2 window + ELF loader + game loop
```

## Dependências (MX Linux / Debian)

```bash
bash install_runtime_deps.sh
```

Instala: `cmake g++ libsdl2-dev libglew-dev libopenal-dev python3`

## Build completo (do zero)

```bash
# 1. Compilar o ps2recomp (se ainda não fez)
cd ..
bash build.sh

# 2. Gerar output.c da ISO
./build/ps2recomp recomp "God of War (USA).iso" build/output.c

# 3. Patch + build do runtime
cd runtime
bash build_runtime.sh
```

## Executar

```bash
# Extrair o ELF da ISO (para carregar dados de RAM correctamente)
./build/ps2recomp extract "God of War (USA).iso" out/
./runtime/build_out/ps2_game out/SCUS_973.99
```

## Ferramentas Python

```bash
# Estatísticas do código gerado
python3 recomp_stats.py ../build/output.c

# Patch manual (build_runtime.sh chama automaticamente)
python3 patch_output.py ../build/output.c ../build/output_runtime.c
```

## Roadmap

| Status | Item |
|--------|------|
| ✅ | ISO 9660 parser |
| ✅ | ELF32 MIPS loader |
| ✅ | Disassembler MIPS R5900 |
| ✅ | Recompilador estático MIPS → C |
| ✅ | GS stub → OpenGL 3.3 (primitivas, cor, scissor, depth) |
| ✅ | BIOS/EE syscall stubs |
| ✅ | SPU2 stub (silent) |
| ✅ | Host SDL2 + ELF loader |
| 🔲 | Textura real (upload GS VRAM → GL texture) |
| 🔲 | Input DualShock 2 → SDL_GameController |
| 🔲 | SPU2 → OpenAL (voices, ADSR, reverb) |
| 🔲 | Dispatch indireto (jalr — chamadas por ponteiro) |
| 🔲 | Instruções MMI e VU0 (operações vetoriais) |
| 🔲 | GS VRAM texel transfer (BITBLTBUF/TRXDIR) |
| 🔲 | IOP/SIF completo (módulo loader) |
