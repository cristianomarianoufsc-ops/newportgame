#!/bin/bash
# run.sh — Atalho para rodar o ps2recomp
# Uso: ./run.sh <comando> <iso> [args...]
# Exemplo: ./run.sh info /home/user/gow.iso

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/build/ps2recomp"

if [ ! -f "$BIN" ]; then
    echo "Binário não encontrado. Rode primeiro: ./build.sh"
    exit 1
fi

"$BIN" "$@"
