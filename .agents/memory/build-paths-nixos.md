---
name: Build paths NixOS
description: Caminhos hardcodados de SDL2, GLEW, GLU, libglvnd e OpenAL extraídos do CMakeCache. Usar quando build_relink.sh falhar por hash do nix store desatualizado.
---

# Build paths no NixOS do Replit (extraídos do CMakeCache.txt)

Se o ambiente NixOS for recriado, os hashes mudam. Rode cmake para regenerar e atualize build_relink.sh.

```bash
cmake -S tools/ps2recomp/runtime -B tools/ps2recomp/runtime/build
grep -E "SDL2|GLEW|GLU|OpenAL|glvnd" tools/ps2recomp/runtime/build/CMakeCache.txt
```

## Paths correntes (julho 2026)

| Biblioteca | Path |
|---|---|
| SDL2 headers | `/nix/store/6vl9b59i822mh3zmri5g4kywahzhp5zw-sdl2-compat-2.32.56-dev/include` |
| SDL2 .so | `/nix/store/az1wz2fh6y0j0simjsl2b58ksyrjdf71-sdl2-compat-2.32.56/lib/libSDL2.so` |
| GLEW headers | `/nix/store/sijv03md3y5v8prmg8xk3fm0p886ajld-glew-2.2.0-dev/include` |
| GLEW .so | `/nix/store/ywxv6wj9pvbrhr7pxybp8plcyz79j34r-glew-2.2.0/lib/libGLEW.so.2.2.0` |
| GL headers (libglvnd-dev) | `/nix/store/akn28bf4vh2q3p2czwkm37acmf33bvgd-libglvnd-1.7.0-dev/include` |
| GLU headers | `/nix/store/lnrbx5fy918mhbxci2ss4ri83xzca0a1-glu-9.0.3-dev/include` |
| libGLX.so | `/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libGLX.so` |
| libOpenGL.so | `/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libOpenGL.so` |
| libGL.so | `/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libGL.so` |
| libGLU.so | `/nix/store/7xv0lc4y7ixbpib2vxna7hd711qp9da2-glu-9.0.3/lib/libGLU.so` |
| libEGL.so | `/nix/store/7227amwg7k4sbl6mhglq17v5x5ki54ks-libglvnd-1.7.0/lib/libEGL.so` |
| OpenAL .so | `/nix/store/91dcv6yfmlhs75q5fckc060j7fya06yv-openal-soft-1.24.2/lib/libopenal.so` |

**Why:** detecção dinâmica de GL no Replit falha porque libGL.so.1 do libglvnd está no formato errado para link direto. Os paths hardcodados apontam para a versão correta identificada pelo CMakeCache.

**How to apply:** Se `bash build_relink.sh` falhar com "lib não encontrada", rode cmake novamente e atualize os paths no topo do script.
