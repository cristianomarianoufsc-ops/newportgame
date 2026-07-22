---
name: ISO download
description: Como baixar a ISO de God of War (USA) do Google Drive no Replit. wget com confirm=t funciona; pip/gdown não disponível neste Python.
---

# Download da ISO de God of War (USA)

## Destino

```
/home/runner/workspace/newportgame/newportgame/tools/ps2recomp/build/God of War (USA).iso
```

Tamanho esperado: 8.522.792.960 bytes (~8.52 GB). Não versionada no git.

## Método funcional: wget com confirm=t

```bash
FILE_ID="1ruRDjG5J0FrCVSU1WdNQqehIoT7csS0S"
DEST="/home/runner/workspace/newportgame/newportgame/tools/ps2recomp/build/God of War (USA).iso"

nohup wget \
    --continue \
    --no-check-certificate \
    --progress=dot:giga \
    --tries=999 \
    --wait=3 \
    --timeout=120 \
    --read-timeout=300 \
    -O "$DEST" \
    "https://drive.usercontent.google.com/download?id=${FILE_ID}&export=download&confirm=t" \
    > /tmp/iso_wget.log 2>&1 &
```

O `--continue` permite retomar se o processo for interrompido.

## Método alternativo: Python urllib

Script em `/tmp/iso_dl.py` — faz retry automático e logging a cada 30s.

```bash
PYTHON=/nix/store/010r29jy64nj14dx7fabacypr4f2q077-python3-3.11.9-env/bin/python3
nohup $PYTHON /tmp/iso_dl.py </dev/null >/dev/null 2>&1 &
```

## O que NÃO funciona

- `pip install gdown` — pip não disponível no Python 3.11 do nix store
- `curl` com cookie + confirm — retorna HTML (confirmation page) em vez do binário

**Why:** O Google Drive exige `confirm=t` para arquivos grandes (virus scan bypass). O wget lida corretamente com redirecionamentos e cookies internamente.

**How to apply:** Se o arquivo existir mas estiver incompleto, usar `--continue` para retomar. Verificar tamanho com `ls -lh`.
