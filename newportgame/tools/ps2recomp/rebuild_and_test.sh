#!/usr/bin/env bash
# rebuild_and_test.sh — pipeline completo do ps2recomp num comando só.
#
# Etapas (puláveis por flag):
#   1. build.sh            — recompila o recompilador (g++ direto, sem cmake)
#   2. recompelf           — regenera build/output_runtime.c a partir do ELF
#   3. compila runtime     — objetos manuais (cmake quebrado no Nix; usa
#                            flags.make + link.txt já corrigidos do build dir)
#   4. teste headless      — roda com PS2_TRACE_SYSCALLS + PC-sampler e
#                            imprime o resumo (syscalls únicos, frames,
#                            gs_writes, endereço quente do spin)
#
# Uso:
#   ./rebuild_and_test.sh                 # pipeline completo
#   ./rebuild_and_test.sh --skip-recomp   # só runtime + teste (mudou .cpp/.h)
#   ./rebuild_and_test.sh --skip-test     # só compila
#   TRACE=10 FRAMES=60 ./rebuild_and_test.sh   # ajustar teste
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

ELF="build/elf_out/SCUS_973.99"
RUNTIME_BUILD="runtime/build"
OBJDIR="$RUNTIME_BUILD/CMakeFiles/ps2_game.dir"
# Caminho legado esperado pelo flags.make (symlinked para o real)
LEGACY_C="/home/runner/workspace/tools/ps2recomp/build/output_runtime.c"
TRACE="${TRACE:-5}"
FRAMES="${FRAMES:-30}"

SKIP_RECOMP=0; SKIP_TEST=0
for a in "$@"; do
    case "$a" in
        --skip-recomp) SKIP_RECOMP=1 ;;
        --skip-test)   SKIP_TEST=1 ;;
    esac
done

if [ "$SKIP_RECOMP" = 0 ]; then
    echo "=== [1/4] Recompilador ==="
    bash build.sh >/dev/null
    echo "OK: build/ps2recomp"

    echo "=== [2/4] Regenerando output_runtime.c ==="
    [ -f "$ELF" ] || { echo "ERRO: ELF ausente: $ELF"; exit 1; }
    ./build/ps2recomp recompelf "$ELF" build/output_runtime.c | tail -1
else
    echo "=== [1-2/4] pulado (--skip-recomp) ==="
fi

echo "=== [3/4] Runtime ==="
# Sincronizar headers/fonte no caminho legado que o flags.make referencia
mkdir -p "$(dirname "$LEGACY_C")" /home/runner/workspace/tools/ps2recomp/runtime/include
ln -sf "$SCRIPT_DIR/build/output_runtime.c" "$LEGACY_C"
for f in "$SCRIPT_DIR"/runtime/include/*.h; do
    ln -sf "$f" "/home/runner/workspace/tools/ps2recomp/runtime/include/$(basename "$f")"
done

# Extrair flags do cmake congelado
C_DEFINES=$(grep  "^C_DEFINES"    "$OBJDIR/flags.make" | sed 's/^C_DEFINES = //')
C_INCLUDES=$(grep "^C_INCLUDES"   "$OBJDIR/flags.make" | sed 's/^C_INCLUDES = //')
C_FLAGS=$(grep    "^C_FLAGS"      "$OBJDIR/flags.make" | sed 's/^C_FLAGS = //')
CXX_DEFINES=$(grep  "^CXX_DEFINES"  "$OBJDIR/flags.make" | sed 's/^CXX_DEFINES = //')
CXX_INCLUDES=$(grep "^CXX_INCLUDES" "$OBJDIR/flags.make" | sed 's/^CXX_INCLUDES = //')
CXX_FLAGS=$(grep    "^CXX_FLAGS"    "$OBJDIR/flags.make" | sed 's/^CXX_FLAGS = //')

cd "$RUNTIME_BUILD"
for src in bios_stub gs_stub host_main spu2_stub; do
    g++ $CXX_DEFINES $CXX_INCLUDES $CXX_FLAGS -c "../src/$src.cpp" \
        -o "CMakeFiles/ps2_game.dir/src/$src.cpp.o"
done
gcc $C_DEFINES $C_INCLUDES $C_FLAGS -c ../src/ps2_runtime_data.c \
    -o CMakeFiles/ps2_game.dir/src/ps2_runtime_data.c.o
gcc -DPS2_RECOMP_HAS_HOST $C_DEFINES $C_INCLUDES $C_FLAGS -c "$LEGACY_C" \
    -o "CMakeFiles/ps2_game.dir$LEGACY_C.o" 2>&1 | grep -E "error" | head -5 || true
[ -f "CMakeFiles/ps2_game.dir$LEGACY_C.o" ] || { echo "ERRO: output_runtime.c não compilou"; exit 1; }
bash -c "$(cat CMakeFiles/ps2_game.dir/link.txt)"
echo "OK: $RUNTIME_BUILD/ps2_game ($(stat -c%s ps2_game) bytes)"

if [ "$SKIP_TEST" = 0 ]; then
    echo "=== [4/4] Teste headless (frames=$FRAMES trace=$TRACE) ==="
    PS2_TRACE_SYSCALLS=$TRACE timeout 60 ./ps2_game --headless --frames=$FRAMES \
        "../../build/elf_out/SCUS_973.99" 2>&1 | tail -30 || true
else
    echo "=== [4/4] pulado (--skip-test) ==="
fi
