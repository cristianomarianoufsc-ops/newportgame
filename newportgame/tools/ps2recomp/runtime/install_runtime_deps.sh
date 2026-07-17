#!/bin/bash
# install_runtime_deps.sh — Instala dependências do runtime ps2_game
# Testado em: MX Linux 23, Debian 12, Ubuntu 22.04
set -e

echo "=== PS2 Runtime — instalando dependências ==="

if ! command -v apt &>/dev/null; then
    echo "ERRO: este script requer apt (Debian / Ubuntu / MX Linux)"
    exit 1
fi

sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    g++ \
    make \
    pkg-config \
    libsdl2-dev \
    libglew-dev \
    libgl1-mesa-dev \
    libopenal-dev \
    python3 \
    python3-pip

echo ""
echo "=== Dependências instaladas! ==="
echo "Agora rode: bash build_runtime.sh"
