#!/bin/bash
# build.sh — Compila o ps2recomp no Linux
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== PS2 Recompiler — Build ==="
echo "Diretório: $SCRIPT_DIR"

# Verifica dependências
for cmd in cmake g++ make; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERRO: '$cmd' não encontrado. Rode primeiro: ./install_deps.sh"
        exit 1
    fi
done

echo "Compilador: $(g++ --version | head -1)"
echo "CMake:      $(cmake --version | head -1)"
echo ""

# Build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++

make -j$(nproc)

echo ""
echo "=== Build concluído! ==="
echo ""
echo "Binário: $BUILD_DIR/ps2recomp"
echo ""
echo "Exemplos de uso:"
echo "  ./build/ps2recomp info    <game.iso>"
echo "  ./build/ps2recomp list    <game.iso>"
echo "  ./build/ps2recomp disasm  <game.iso> 100"
echo "  ./build/ps2recomp extract <game.iso> ./out/"
echo "  ./build/ps2recomp recomp  <game.iso> output.c"
