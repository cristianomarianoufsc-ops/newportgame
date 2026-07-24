#!/bin/bash
# build_relink.sh — Recompila apenas os .cpp do runtime e religa sem cmake.
#
# Funciona no Replit mesmo quando cmake está quebrado ou o cache aponta para path errado.
# Não recompila output_runtime.c (operação cara — ±5min).
#
# Paths de bibliotecas extraídos do CMakeCache.txt e link.txt do build anterior
# (que funcionou) — hardcodados para evitar detecção dinâmica falha no Replit NixOS.
#
# Uso:
#   bash tools/ps2recomp/build_relink.sh          # recompila runtime + religa
#   bash tools/ps2recomp/build_relink.sh --full   # + recompila output_runtime.c

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RT="$SCRIPT_DIR/runtime"
BD="$SCRIPT_DIR/build"
OBJ="$RT/build/CMakeFiles/ps2_game.dir"

echo "=== build_relink.sh ==="
echo "  RT : $RT"
echo "  BD : $BD"
echo ""

# -----------------------------------------------------------------------
# Paths hardcodados (do CMakeCache.txt + link.txt do build cmake anterior)
# Se mudar o ambiente NixOS, atualizar aqui e re-executar cmake primeiro.
# -----------------------------------------------------------------------
SDL2_INC="/nix/store/6vl9b59i822mh3zmri5g4kywahzhp5zw-sdl2-compat-2.32.56-dev/include"
SDL2_SO="/nix/store/az1wz2fh6y0j0simjsl2b58ksyrjdf71-sdl2-compat-2.32.56/lib/libSDL2.so"
GLEW_INC="/nix/store/sijv03md3y5v8prmg8xk3fm0p886ajld-glew-2.2.0-dev/include"
GLEW_SO="/nix/store/ywxv6wj9pvbrhr7pxybp8plcyz79j34r-glew-2.2.0/lib/libGLEW.so.2.2.0"
GL_INC="/nix/store/akn28bf4vh2q3p2czwkm37acmf33bvgd-libglvnd-1.7.0-dev/include"
GLU_INC="/nix/store/lnrbx5fy918mhbxci2ss4ri83xzca0a1-glu-9.0.3-dev/include"
OPENAL_SO="/nix/store/91dcv6yfmlhs75q5fckc060j7fya06yv-openal-soft-1.24.2/lib/libopenal.so"
GL_GLX_SO="/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libGLX.so"
GL_OPEN_SO="/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libOpenGL.so"
GL_SO="/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libGL.so"
GLU_SO="/nix/store/7xv0lc4y7ixbpib2vxna7hd711qp9da2-glu-9.0.3/lib/libGLU.so"
EGL_SO="/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libEGL.so"

# Verificação rápida das libs principais
for lib in "$SDL2_SO" "$GLEW_SO" "$OPENAL_SO" "$GL_SO"; do
    if [ ! -f "$lib" ]; then
        echo "ERRO: lib não encontrada: $lib" >&2
        echo "Dica: o hash do nix store pode ter mudado. Rode cmake novamente e" >&2
        echo "      atualize os paths hardcodados neste script." >&2
        echo "      cmake -S $RT -B $RT/build" >&2
        exit 1
    fi
done
echo "  SDL2   : OK ($SDL2_SO)"
echo "  GLEW   : OK"
echo "  OpenAL : OK"
echo "  GL     : OK"
echo ""

# -----------------------------------------------------------------------
# Flags de compilação (mesmas do CMakeLists.txt)
# -----------------------------------------------------------------------
CXX_FLAGS="-O2 -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE=1 -D_REENTRANT"
INC="-I$RT/include -I$SDL2_INC -I$SDL2_INC/SDL2 -I$GLEW_INC -I$GL_INC -I$GLU_INC"

# Garante que o diretório de objetos existe
mkdir -p "$OBJ/src"

# -----------------------------------------------------------------------
# Compila fontes C++ do runtime
# -----------------------------------------------------------------------
echo "=== Compilando fontes C++ do runtime ==="
for src in gs_stub bios_stub spu2_stub host_main; do
    echo -n "  $src.cpp ... "
    g++ $CXX_FLAGS $INC -c "$RT/src/$src.cpp" -o "$OBJ/src/$src.cpp.o" && echo "OK"
done

# -----------------------------------------------------------------------
# Compila override_stubs.c (sistema de interceptação por endereço)
# -----------------------------------------------------------------------
if [ -f "$RT/src/override_stubs.c" ]; then
    echo -n "  override_stubs.c ... "
    C_FLAGS="-O2 -std=gnu11 -Wall -Wno-unused-function -D_GNU_SOURCE=1"
    gcc $C_FLAGS -I"$RT/include" -c "$RT/src/override_stubs.c" -o "$OBJ/src/override_stubs.c.o" && echo "OK"
    OVERRIDE_OBJ="$OBJ/src/override_stubs.c.o"
else
    OVERRIDE_OBJ=""
fi

# -----------------------------------------------------------------------
# Opcionalmente recompila output_runtime.c (lento — ±5min)
# -----------------------------------------------------------------------
if [[ "$1" == "--full" ]]; then
    echo "=== Compilando output_runtime.c (pode demorar alguns minutos) ==="
    C_FLAGS="-O1 -std=gnu11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-label -D_GNU_SOURCE=1 -DPS2_RECOMP_HAS_HOST"
    OUTPUT_OBJ=$(find "$OBJ" -name "output_runtime.c.o" 2>/dev/null | head -1)
    if [ -z "$OUTPUT_OBJ" ]; then
        # Cria diretório espelhando o path absoluto (convenção CMake)
        ABS_PATH="${BD}/output_runtime.c"
        MIRROR_DIR="$OBJ${ABS_PATH}"
        MIRROR_DIR="$(dirname "$OBJ/$(realpath --relative-to=/ "$BD/output_runtime.c")")"
        mkdir -p "$MIRROR_DIR"
        OUTPUT_OBJ="$MIRROR_DIR/output_runtime.c.o"
    fi
    gcc $C_FLAGS $INC -c "$BD/output_runtime.c" -o "$OUTPUT_OBJ" && echo "output_runtime.c OK"
fi

# -----------------------------------------------------------------------
# Linka ps2_game
# -----------------------------------------------------------------------
echo "=== Linkando ps2_game ==="
OUTPUT_OBJ=$(find "$OBJ" -name "output_runtime.c.o" 2>/dev/null | head -1)

if [ -z "$OUTPUT_OBJ" ]; then
    echo "ERRO: output_runtime.c.o não encontrado." >&2
    echo "      Execute com --full para recompilar, ou rode cmake primeiro." >&2
    exit 1
fi

# Gera flags --wrap automaticamente para cada função na tabela de overrides
# Formato esperado em override_stubs.c: void __wrap_func_XXXXX(PS2Regs* regs)
WRAP_FLAGS=""
if [ -f "$RT/src/override_stubs.c" ]; then
    while IFS= read -r fn; do
        real="${fn/__wrap_/}"
        WRAP_FLAGS="$WRAP_FLAGS -Wl,--wrap=$real"
    done < <(grep -oP '(?<=void )__wrap_func_[0-9a-fA-F]+' "$RT/src/override_stubs.c")
    [ -n "$WRAP_FLAGS" ] && echo "  Wrap flags:$WRAP_FLAGS"
fi

g++ -O2 \
    "$OBJ/src/gs_stub.cpp.o" \
    "$OBJ/src/bios_stub.cpp.o" \
    "$OBJ/src/spu2_stub.cpp.o" \
    "$OBJ/src/host_main.cpp.o" \
    "$OBJ/src/ps2_runtime_data.c.o" \
    ${OVERRIDE_OBJ:+"$OVERRIDE_OBJ"} \
    "$OUTPUT_OBJ" \
    -o "$RT/build/ps2_game" \
    $WRAP_FLAGS \
    "$SDL2_SO" "$GLEW_SO" -lm -lpthread \
    "$OPENAL_SO" "$GL_GLX_SO" "$GL_OPEN_SO" "$GL_SO" "$GLU_SO" "$EGL_SO" \
    && echo "=== ps2_game OK: $(ls -lh "$RT/build/ps2_game" | awk '{print $5}') ==="

echo ""
echo "Para testar (requer ELF da ISO — ver AGENTS.md):"
echo "  $RT/build/ps2_game --headless --frames 30 $BD/elf_out/SCUS_973.99.elf 2>&1 | tee /tmp/headless.log"
echo "  python3 $SCRIPT_DIR/triage_headless.py /tmp/headless.log"
