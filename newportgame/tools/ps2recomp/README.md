# PS2 Static Recompiler

Ferramenta de recompilação estática de PS2 para PC.  
Lê uma ISO de PS2, extrai o executável MIPS R5900, e traduz para C nativo.

## O que faz

| Etapa | Descrição |
|-------|-----------|
| ISO parser | Lê o filesystem ISO 9660 do disco PS2 |
| ELF loader | Parseia o executável MIPS ELF32 do jogo |
| Disassembler | Decodifica instruções MIPS R5900 (EE) |
| Recompilador | Traduz MIPS → C compilável para x86-64 |

## Requisitos

- Linux (Debian / Ubuntu / MX Linux / Mint)
- g++ 11 ou superior
- cmake 3.16 ou superior
- make

## Instalação rápida (MX Linux)

```bash
# 1. Instalar dependências
chmod +x install_deps.sh
./install_deps.sh

# 2. Compilar
chmod +x build.sh
./build.sh
```

## Uso

```bash
# Ver informações da ISO (SYSTEM.CNF + ELF)
./run.sh info /caminho/para/god_of_war.iso

# Listar todos os arquivos do disco
./run.sh list /caminho/para/god_of_war.iso

# Disassemblar as primeiras 200 instruções do entry point
./run.sh disasm /caminho/para/god_of_war.iso 200

# Extrair o executável ELF para uma pasta
mkdir -p out
./run.sh extract /caminho/para/god_of_war.iso ./out/

# Gerar o C traduzido (passo principal do port)
./run.sh recomp /caminho/para/god_of_war.iso output.c
```

## Arquitetura do projeto

```
src/
├── iso/
│   ├── udf_parser.h / .cpp   — Parser ISO 9660
├── elf/
│   ├── elf_loader.h / .cpp   — Loader ELF32 MIPS
├── mips/
│   ├── disasm.h / .cpp       — Disassembler MIPS R5900
├── recomp/
│   ├── recompiler.h / .cpp   — Tradutor MIPS → C
└── main.cpp                  — CLI principal
```

## Roadmap

- [x] ISO 9660 parser
- [x] ELF32 MIPS loader + imagem de memória
- [x] Disassembler MIPS R5900 completo
- [x] Recompilador estático MIPS → C
- [ ] Stub do Graphics Synthesizer (GS) → OpenGL
- [ ] Stub do IOP/BIOS (syscalls do PS2)
- [ ] Stub do SPU2 (áudio → OpenAL)
- [ ] Dispatch indireto (jalr — chamadas por ponteiro)
- [ ] Instruções MMI e VU0 (operações vetoriais)
- [ ] Loader do segmento ELF no ps2_ram em runtime

## Exemplo de output do disassembler

```
001f0000  27bdffd0   addiu       $sp, $sp, -48
001f0004  afbf002c   sw          $ra, 44($sp)
001f0008  afbe0028   sw          $fp, 40($sp)
001f000c  03a0f021   move        $fp, $sp
001f0010  0c07c3f0   jal         0x01f0fc0
```

## Exemplo de output do recompilador (output.c)

```c
void func_001f0000(PS2Regs* regs) {
    // addiu $sp, $sp, -48
    regs->r[29] = (uint32_t)((int32_t)regs->r[29] + -48);
    // sw $ra, 44($sp)
    mem_write32((uint32_t)(regs->r[29] + 44), (uint32_t)regs->r[31]);
    // jal 0x01f0fc0
    regs->r[31] = 0x1f0014u; func_001f0fc0(&regs);
    ...
}
```
