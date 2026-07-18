# shell.nix — Ambiente Nix para compilar o runtime ps2_game
# Uso: nix-shell --run "bash build_runtime.sh"
# (ou simplesmente: nix-shell  para entrar no shell interativo)
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  buildInputs = with pkgs; [
    cmake
    gcc
    gnumake
    pkg-config
    SDL2
    glew
    openal
    mesa             # libGL / libGLU
    mesa.dev
    libGL
    libGLU
  ];

  shellHook = ''
    echo "=== ps2_game build environment ==="
    echo "g++:     $(g++ --version | head -1)"
    echo "cmake:   $(cmake --version | head -1)"
    echo "SDL2:    $(pkg-config --modversion sdl2 2>/dev/null || echo 'n/a')"
    echo "GLEW:    $(pkg-config --modversion glew  2>/dev/null || echo 'n/a')"
    echo ""
  '';
}
