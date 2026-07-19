#!/bin/bash
# build_relink.sh — Recompila apenas os .cpp do runtime e religar sem cmake.
# Funciona no Replit mesmo quando cmake está quebrado ou o cache aponta para path errado.
# Não recompila output_runtime.c (operação cara — ±5min).
#
# Uso:
#   bash tools/ps2recomp/build_relink.sh          # recompila runtime + relinká
#   bash tools/ps2recomp/build_relink.sh --full   # + recompila output_runtime.c

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RT="$SCRIPT_DIR/runtime"
BD="$SCRIPT_DIR/build"
OBJ="$RT/build/CMakeFiles/ps2_game.dir"

# Detecta paths de SDL2 e OpenAL via ldd do binário existente (se houver)
detect_lib() {
    local pattern="$1"
    ldd "$RT/build/ps2_game" 2>/dev/null | grep -oP "(?<=> ).*$pattern[^\s]*" | head -1 | xargs readlink -f 2>/dev/null || true
}

# SDL2 headers (pacote -dev)
SDL2_DEV_INC=$(ls -d /nix/store/*sdl2*compat*dev*/include/SDL2 2>/dev/null \
               || ls -d /nix/store/*SDL2*dev*/include/SDL2 2>/dev/null \
               || echo "")

# SDL2 runtime .so (detectado via ldd do binário já compilado)
SDL2_SO=$(detect_lib "libSDL2")
if [ -z "$SDL2_SO" ]; then
    SDL2_SO=$(ls /nix/store/*sdl2*compat*/lib/libSDL2-2.0.so.0* 2>/dev/null | grep -v dev | head -1)
fi

# OpenAL
OPENAL_INC=$(ls -d /nix/store/*openal*/include 2>/dev/null | head -1 || echo "")
OPENAL_SO=$(ls /nix/store/*openal*/lib/libopenal.so 2>/dev/null | head -1 || echo "")

# GLEW / GL
GLEW_SO=$(ls /nix/store/*glew*/lib/libGLEW.so.2* 2>/dev/null | head -1 || echo "")
GL_SO=$(ls /nix/store/*libglvnd*/lib/libGL.so.1 2>/dev/null | head -1 \
        || ls /nix/store/*libglvnd*/lib/libGL.so 2>/dev/null | head -1 || echo "")

echo "=== build_relink.sh ==="
echo "  RT     : $RT"
echo "  SDL2   : ${SDL2_SO:-NÃO ENCONTRADO}"
echo "  GLEW   : ${GLEW_SO:-NÃO ENCONTRADO}"
echo "  OpenAL : ${OPENAL_SO:-NÃO ENCONTRADO}"
echo ""

if [ -z "$SDL2_DEV_INC" ]; then
    echo "ERRO: SDL2 headers não encontrados." >&2
    exit 1
fi

# -----------------------------------------------------------------------
# Flags de compilação
# -----------------------------------------------------------------------
CXX_FLAGS="-O2 -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE=1 -D_REENTRANT"
INC="-I$RT/include -I$SDL2_DEV_INC ${OPENAL_INC:+-I$OPENAL_INC}"

# -----------------------------------------------------------------------
# Compila fontes C++ do runtime
# -----------------------------------------------------------------------
echo "=== Compilando fontes C++ do runtime ==="
for src in gs_stub bios_stub spu2_stub host_main; do
    echo -n "  $src.cpp ... "
    g++ $CXX_FLAGS $INC -c "$RT/src/$src.cpp" -o "$OBJ/src/$src.cpp.o" && echo "OK"
done

# -----------------------------------------------------------------------
# Opcionalmente recompila output_runtime.c (lento)
# -----------------------------------------------------------------------
if [[ "$1" == "--full" ]]; then
    echo "=== Compilando output_runtime.c (pode demorar alguns minutos) ==="
    C_FLAGS="-O1 -std=gnu11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-label -D_GNU_SOURCE=1"
    OUTPUT_OBJ=$(find "$OBJ" -name "output_runtime.c.o" | head -1)
    if [ -z "$OUTPUT_OBJ" ]; then
        OUTPUT_OBJ="$OBJ/output_runtime.c.o"
    fi
    gcc $C_FLAGS $INC -c "$BD/output_runtime.c" -o "$OUTPUT_OBJ" && echo "output_runtime.c OK"
fi

# -----------------------------------------------------------------------
# Linka ps2_game
# -----------------------------------------------------------------------
echo "=== Linkando ps2_game ==="
OUTPUT_OBJ=$(find "$OBJ" -name "output_runtime.c.o" | head -1)

LIBS=""
[ -n "$SDL2_SO"   ] && LIBS="$LIBS $SDL2_SO"
[ -n "$GLEW_SO"   ] && LIBS="$LIBS $GLEW_SO"
[ -n "$OPENAL_SO" ] && LIBS="$LIBS $OPENAL_SO"
[ -n "$GL_SO"     ] && LIBS="$LIBS $GL_SO"
LIBS="$LIBS -lm -lpthread"

g++ -O2 \
    "$OBJ/src/gs_stub.cpp.o" \
    "$OBJ/src/bios_stub.cpp.o" \
    "$OBJ/src/spu2_stub.cpp.o" \
    "$OBJ/src/host_main.cpp.o" \
    "$OBJ/src/ps2_runtime_data.c.o" \
    "$OUTPUT_OBJ" \
    -o "$RT/build/ps2_game" \
    $LIBS \
    && echo "=== ps2_game OK: $(ls -lh $RT/build/ps2_game | awk '{print $5}') ==="

echo ""
echo "Para testar:"
echo "  $RT/build/ps2_game --headless --frames 30 $BD/elf_out/SCUS_973.99.elf"
