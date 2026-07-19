#!/usr/bin/env bash
# PS2 Recompiler — Build script
# Usa g++ direto (cmake está quebrado no Replit/NixOS)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== PS2 Recompiler — Build ==="
echo "Diretório: $SCRIPT_DIR"

# Verificar g++
if ! command -v g++ &>/dev/null; then
    echo "ERRO: g++ não encontrado."
    exit 1
fi

GXX_VER=$(g++ --version | head -1)
echo "Compilador: $GXX_VER"

mkdir -p build

echo "Compilando..."
g++ -std=c++20 -O2 -Wall -Wextra \
    src/main.cpp \
    src/iso/udf_parser.cpp \
    src/elf/elf_loader.cpp \
    src/mips/disasm.cpp \
    src/recomp/recompiler.cpp \
    -I src \
    -o build/ps2recomp

echo ""
echo "=== Build concluído ==="
echo "Binário: build/ps2recomp"
echo ""
echo "Uso:"
echo "  ./build/ps2recomp info   \"God of War (USA).iso\""
echo "  ./build/ps2recomp recomp \"God of War (USA).iso\" build/output.c"
