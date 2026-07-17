#!/bin/bash
# build_runtime.sh — Compila o runtime ps2_game no Linux
# Requer: bash install_runtime_deps.sh executado antes
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_out"

echo "=== PS2 Runtime — Build ==="
echo "Diretório: $SCRIPT_DIR"
echo ""
echo "DICA: Se as dependências não forem encontradas, use:"
echo "  nix-shell shell.nix --run 'bash build_runtime.sh'"
echo "  (ou em MX Linux: bash install_runtime_deps.sh  antes)"
echo ""

# Verifica que output.c existe
OUTPUT_C="$SCRIPT_DIR/../build/output.c"
if [ ! -f "$OUTPUT_C" ]; then
    echo "ERRO: output.c não encontrado em $OUTPUT_C"
    echo "Execute primeiro: cd ../.. && bash build.sh && ./build/ps2recomp recomp 'God of War (USA).iso' build/output.c"
    exit 1
fi

# Verifica cmake e g++
for cmd in cmake g++ make pkg-config; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERRO: '$cmd' não encontrado. Rode: bash install_runtime_deps.sh"
        exit 1
    fi
done

echo "Compilador: $(g++ --version | head -1)"
echo "CMake:      $(cmake --version | head -1)"
echo "output.c:   $(wc -l < "$OUTPUT_C") linhas"
echo ""

# Aplicar patch no output.c → output_runtime.c
PATCHED_C="$SCRIPT_DIR/../build/output_runtime.c"
echo "Aplicando patch no output.c..."
python3 "$SCRIPT_DIR/patch_output.py" "$OUTPUT_C" "$PATCHED_C"
echo ""

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc

make -j"$(nproc)"

echo ""
echo "=== Build concluído! ==="
echo ""
echo "Binário: $BUILD_DIR/ps2_game"
echo ""
echo "Como rodar:"
echo "  # Apenas código recompilado (sem dados de RAM):"
echo "  $BUILD_DIR/ps2_game"
echo ""
echo "  # Com ELF extraído (carrega dados de RAM corretamente):"
echo "  $BUILD_DIR/ps2_game path/to/SCUS_973.99.elf"
echo ""
echo "  # Extrair o ELF da ISO primeiro:"
echo "  mkdir -p out"
echo "  $SCRIPT_DIR/../build/ps2recomp extract 'God of War (USA).iso' out/"
echo "  $BUILD_DIR/ps2_game out/SCUS_973.99"
