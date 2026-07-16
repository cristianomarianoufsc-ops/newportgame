#!/bin/bash
# install_deps.sh — Instala dependências do ps2recomp no Debian/Ubuntu/MX Linux
set -e

echo "=== PS2 Recompiler — instalando dependências ==="

if ! command -v apt &>/dev/null; then
    echo "ERRO: este script é para sistemas baseados em Debian/Ubuntu/MX Linux"
    exit 1
fi

sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    g++ \
    make \
    git

echo ""
echo "=== Dependências instaladas com sucesso! ==="
echo "Agora rode: ./build.sh"
