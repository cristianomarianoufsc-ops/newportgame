#!/usr/bin/env python3
"""
disassemble_elf.py — Desassemblador estático do ELF PS2 (MIPS R5900 / EE)
==========================================================================
Adaptado do projeto godofwar-main para o pipeline ps2recomp.
Elimina a necessidade de Ghidra para análise pontual de funções.

USO:
    # Desassemblar a função no endereço dado:
    python3 tools/ps2recomp/disassemble_elf.py build/elf_out/SCUS_973.99 --func 0x100008

    # Várias funções de uma vez:
    python3 tools/ps2recomp/disassemble_elf.py build/elf_out/SCUS_973.99 --func 0x100008 0x80000 0x1000c0

    # Alvos padrão do projeto (spin-loops / bloqueadores conhecidos):
    python3 tools/ps2recomp/disassemble_elf.py build/elf_out/SCUS_973.99 --default-targets

    # Só resumo (sem listagem instrução por instrução):
    python3 tools/ps2recomp/disassemble_elf.py build/elf_out/SCUS_973.99 --func 0x100008 --summary-only

    # Salvar em arquivo:
    python3 tools/ps2recomp/disassemble_elf.py build/elf_out/SCUS_973.99 --default-targets > /tmp/disasm_gow.txt

SAÍDA:
    - Assembly MIPS anotado (registradores conhecidos, syscalls, chamadas)
    - Resumo: o que a função lê/escreve/chama, o que retorna em $v0
    - Sugestão de stub HLE baseada na análise

ELF esperado em:
    tools/ps2recomp/build/elf_out/SCUS_973.99   (extraído do ISO via extract_elf_from_gdrive.py)

DEPENDÊNCIAS:
    pip install capstone pyelftools
"""

import sys
import argparse
import struct
import re
from pathlib import Path

# ─── Dependências ────────────────────────────────────────────────────────────

try:
    import capstone
    from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
    from capstone.mips import *
except ImportError:
    print("ERRO: pip install capstone")
    sys.exit(1)

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    print("ERRO: pip install pyelftools")
    sys.exit(1)

# ─── Banco de conhecimento do projeto GOW (SCUS_973.99) ──────────────────────
# Endereços PS2 virtuais → nomes legíveis
# Expandir à medida que novos símbolos forem identificados.

KNOWN_MEM = {
    # Entry / init
    0x00100008: "elf_entry_point",
    0x00100088: "elf_entry_last_insn",   # ponto onde o recompilador trava (mtlo1)

    # SIF / IOP
    0x80000000: "SIF_REG_SMFLAG",        # lido por SifGetReg — bits de status IOP→EE
    0x80000001: "SIF_REG_MAIN_ADDR",     # endereço handler SIF no EE
    0x80000002: "SIF_REG_MSFLAG",        # bits de status EE→IOP

    # GS
    0x12000000: "GS_CSR",               # GS Control Status Register
    0x12001000: "GS_PMODE",
    0x12001010: "GS_SMODE2",
    0x12001040: "GS_DISPFB1",
    0x12001050: "GS_DISPLAY1",
    0x12001060: "GS_DISPFB2",
    0x12001070: "GS_DISPLAY2",
    0x12001080: "GS_EXTBUF",
    0x12001090: "GS_EXTDATA",
    0x120010A0: "GS_EXTWRITE",
    0x120010B0: "GS_BGCOLOR",
    0x120000E0: "GS_BUSDIR",

    # EE scratchpad / common OS structs
    0x70000000: "SPR_base",             # Scratchpad RAM (16KB)
    0x00080000: "kernel_stack_top",     # típico topo de stack do kernel EE
}

KNOWN_FUNCS = {
    # Boot
    0x00100008: "elf_entry",
    # BIOS syscalls (dispatch via 'syscall' instrução)
    # Funções identificadas nos logs de boot
}

# Syscall codes → nomes (EE BIOS)
SYSCALL_NAMES = {
    0x00: "RotateThreadReadyQueue",  # ← o spin-loop que estamos debugando
    0x01: "ResetEE",
    0x02: "SetGsCrt",
    0x03: "InitMainThread",
    0x04: "InitHeap",
    0x05: "EndOfHeap",
    0x20: "CreateThread",
    0x21: "DeleteThread",
    0x22: "StartThread",
    0x23: "ExitThread",
    0x24: "ExitDeleteThread",
    0x40: "SleepThread",
    0x41: "WakeupThread",
    0x45: "iSignalSema",
    0x47: "WaitSema",
    0x50: "CreateSema",
    0x51: "DeleteSema",
    0x52: "SignalSema",
    0x53: "PollSema",
    0x54: "ReferSemaStatus",
    0x60: "SetAlarm",
    0x62: "ReleaseAlarm",
    0x64: "DI",
    0x65: "EI",
    0x68: "FlushCache",
    0x70: "GsGetIMR",
    0x77: "SifSetDma",
    0x78: "SifInitCmd",
    0x79: "SifInitRpc",
    0x7A: "SifSetReg",
    0x7B: "SifGetReg",
    0x3C: "printf",
    0x3D: "dprintf",
}

# Alvos padrão a analisar quando --default-targets é passado.
# Atualizar conforme novos bloqueadores forem identificados.
DEFAULT_TARGETS = [
    (0x00100008, "elf_entry_point"),
    # Adicionar mais bloqueadores aqui quando identificados
]

# ─── ELF parsing ─────────────────────────────────────────────────────────────

def load_segments(elf_path: str):
    """Retorna lista de (vaddr, data) para segmentos PT_LOAD do ELF."""
    segments = []
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        for seg in elf.iter_segments():
            if seg.header.p_type == "PT_LOAD" and seg.header.p_filesz > 0:
                vaddr = seg.header.p_vaddr
                data  = seg.data()
                segments.append((vaddr, data))
    return segments

def read_vaddr(segments, vaddr: int, size: int):
    """Lê `size` bytes da memória virtual PS2. Retorna None se não mapeado."""
    for base, data in segments:
        if base <= vaddr < base + len(data):
            off = vaddr - base
            if off + size <= len(data):
                return data[off:off + size]
    return None

# ─── Disassembler ─────────────────────────────────────────────────────────────

# Capstone: MIPS32 little-endian (R5900 é subset do MIPS32)
_cs = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
_cs.detail = True
_cs.skipdata = True   # não parar em bytes inválidos

MAX_INSNS = 300   # limite por função para não explodir

def _reg_name(rid: int) -> str:
    """Retorna o nome simbólico do registrador MIPS."""
    MIPS_REGS = [
        "$zero","$at","$v0","$v1","$a0","$a1","$a2","$a3",
        "$t0","$t1","$t2","$t3","$t4","$t5","$t6","$t7",
        "$s0","$s1","$s2","$s3","$s4","$s5","$s6","$s7",
        "$t8","$t9","$k0","$k1","$gp","$sp","$fp","$ra",
    ]
    return MIPS_REGS[rid] if 0 <= rid < 32 else f"$r{rid}"

def disassemble_function(segments, start_vaddr: int, max_insns: int = MAX_INSNS):
    """
    Desassembla instruções a partir de start_vaddr até encontrar
    JR $ra, ERET, ou max_insns atingido.
    Retorna lista de (vaddr, mnemonic, op_str, raw_bytes, annotation).
    """
    result = []
    vaddr  = start_vaddr
    seen   = set()

    for _ in range(max_insns):
        if vaddr in seen:
            break
        seen.add(vaddr)

        raw = read_vaddr(segments, vaddr, 4)
        if raw is None:
            result.append((vaddr, "???", "<not mapped>", b"", ""))
            break

        insns = list(_cs.disasm(raw, vaddr))
        if not insns:
            result.append((vaddr, ".word", f"0x{struct.unpack('<I', raw)[0]:08x}", raw, ""))
            vaddr += 4
            continue

        insn = insns[0]
        ann  = _annotate(insn, segments)
        result.append((vaddr, insn.mnemonic, insn.op_str, raw, ann))

        # Pára em JR $ra, JR $k0, ERET
        if insn.mnemonic in ("jr", "eret"):
            # Executa delay slot também
            delay_raw = read_vaddr(segments, vaddr + 4, 4)
            if delay_raw:
                di = list(_cs.disasm(delay_raw, vaddr + 4))
                if di:
                    dann = _annotate(di[0], segments)
                    result.append((vaddr + 4, di[0].mnemonic, di[0].op_str, delay_raw, dann + " [delay]"))
            break

        vaddr += insn.size

    return result

def _annotate(insn, segments) -> str:
    """Adiciona anotação de contexto à instrução (endereço simbólico, syscall, etc)."""
    ann_parts = []

    mnem = insn.mnemonic.lower()

    # Detecta SYSCALL
    if mnem == "syscall":
        # O código do syscall fica nos bits 25:6 da instrução raw
        raw_word = struct.unpack("<I", insn.bytes)[0]
        code = (raw_word >> 6) & 0xFFFFF
        name = SYSCALL_NAMES.get(code, "?")
        ann_parts.append(f"→ SYSCALL 0x{code:02x} ({name})")

    # Detecta acesso a memória com endereço simbólico no imediato
    # ex: lw $v0, 0x1234($zero) → lê de PS2 vaddr 0x1234
    if hasattr(insn, "operands"):
        for op in insn.operands:
            if op.type == capstone.mips.MIPS_OP_MEM:
                base_val = 0
                if op.mem.base == capstone.mips.MIPS_REG_ZERO:
                    base_val = 0
                elif op.mem.base == capstone.mips.MIPS_REG_GP:
                    base_val = None  # GP-relative, não sabemos o valor de GP aqui
                if base_val is not None:
                    eff = (base_val + op.mem.disp) & 0xFFFFFFFF
                    sym = KNOWN_MEM.get(eff)
                    if sym:
                        ann_parts.append(f"[{sym}]")

    # Detecta branch/jump para endereço conhecido
    if mnem in ("j", "jal", "bal"):
        if hasattr(insn, "operands") and insn.operands:
            target = insn.operands[0].imm
            sym = KNOWN_FUNCS.get(target) or KNOWN_MEM.get(target)
            if sym:
                ann_parts.append(f"→ {sym}")

    return "  ; " + ", ".join(ann_parts) if ann_parts else ""

# ─── Análise de função ────────────────────────────────────────────────────────

def analyze_function(disasm):
    """
    Extrai padrões relevantes do disassembly:
    - Syscalls chamadas
    - Endereços de memória acessados
    - Funções chamadas (JAL)
    - Valor retornado em $v0 (último ADDIU/LI/ORI $v0, ...)
    - Presença de spin-loop (branch para si mesmo ou para instrução anterior)
    """
    syscalls      = []
    mem_reads     = []
    mem_writes    = []
    calls         = []
    v0_last       = None
    spin_suspects = []

    addrs = [va for va, *_ in disasm]

    for i, (va, mnem, ops, raw, ann) in enumerate(disasm):
        mnem = mnem.lower()

        # Syscalls
        if mnem == "syscall" and raw:
            raw_word = struct.unpack("<I", raw)[0]
            code = (raw_word >> 6) & 0xFFFFF
            syscalls.append((va, code, SYSCALL_NAMES.get(code, "?")))

        # JAL / BAL calls
        if mnem in ("jal", "bal") and ops:
            try:
                target = int(ops.strip().split()[0], 0)
                calls.append((va, target, KNOWN_FUNCS.get(target, "")))
            except Exception:
                pass

        # Últimas atribuições a $v0
        if "$v0" in ops and mnem in ("addiu","ori","lui","li","move","addu","add","xor"):
            v0_last = (va, mnem, ops)

        # Spin-loop: branch cujo alvo é um endereço anterior (ou o mesmo)
        if mnem.startswith("b") and ops:
            parts = ops.split(",")
            target_str = parts[-1].strip()
            try:
                target = int(target_str, 0)
                if target in addrs and target <= va:
                    distance = va - target
                    spin_suspects.append((va, target, distance))
            except Exception:
                pass

    return {
        "syscalls":      syscalls,
        "calls":         calls,
        "v0_last":       v0_last,
        "spin_suspects": spin_suspects,
    }

# ─── Saída ────────────────────────────────────────────────────────────────────

def print_disasm(disasm, start_vaddr: int, label: str):
    print(f"\n  {'─'*68}")
    print(f"  DISASSEMBLY: 0x{start_vaddr:08X}  {label}")
    print(f"  {'─'*68}")
    for va, mnem, ops, raw, ann in disasm:
        sym = KNOWN_FUNCS.get(va, "")
        sym_str = f"  <{sym}>" if sym else ""
        hex_bytes = raw.hex() if raw else "????????"
        print(f"  0x{va:08x}  {hex_bytes}  {mnem:<8} {ops:<30}{ann}{sym_str}")

def print_summary(analysis: dict, start_vaddr: int):
    print(f"\n  SUMMARY 0x{start_vaddr:08X}:")

    if analysis["syscalls"]:
        print(f"  Syscalls chamados:")
        for va, code, name in analysis["syscalls"]:
            print(f"    0x{va:08x}  SYSCALL 0x{code:02x} ({name})")

    if analysis["calls"]:
        print(f"  Funções chamadas (JAL/BAL):")
        for va, target, label in analysis["calls"]:
            lbl = f"  [{label}]" if label else ""
            print(f"    0x{va:08x}  → 0x{target:08x}{lbl}")

    if analysis["v0_last"]:
        va, mnem, ops = analysis["v0_last"]
        print(f"  Último valor de $v0 setado:")
        print(f"    0x{va:08x}  {mnem} {ops}")

    if analysis["spin_suspects"]:
        print(f"  ⚠️  SPIN-LOOP SUSPEITO:")
        for va, target, dist in analysis["spin_suspects"]:
            print(f"    Branch em 0x{va:08x} → 0x{target:08x} "
                  f"(dist={dist} bytes, {dist//4} insns)")
            # Identifica o que a função está esperando
            print(f"    → DIAGNÓSTICO: função provavelmente spin-waita em alguma flag")
            print(f"      Verifique qual registrador/endereço de memória muda o branch")

def suggest_hle_stub(analysis: dict, start_vaddr: int):
    """Sugere um stub HLE para substituir a função, baseado na análise."""
    print(f"\n  SUGESTÃO DE STUB HLE para 0x{start_vaddr:08X}:")

    if analysis["spin_suspects"]:
        print(f"  // Esta função contém um spin-loop.")
        print(f"  // No stub: retornar diretamente para evitar travamento.")
        print(f"  // Adicione em bios_stub.cpp se for chamada por JALR:")
        print(f"  //   case 0x{start_vaddr:06x}:")
        print(f"  //     regs->r[2] = 1;  // v0 = sucesso")
        print(f"  //     break;")
    elif analysis["syscalls"]:
        sc = analysis["syscalls"]
        print(f"  // Chama {len(sc)} syscall(s): "
              f"{', '.join(SYSCALL_NAMES.get(c,'?') for _,c,_ in sc)}")
        print(f"  // Garantir que bios_stub.cpp trate esses códigos corretamente.")
    else:
        print(f"  // Função sem syscalls ou spin-loops identificados.")
        print(f"  // Provavelmente é código de setup de registradores/structs.")

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("elf", nargs="?",
                    help="Caminho para SCUS_973.99 (auto-detectado se omitido)")
    ap.add_argument("--func", nargs="+", metavar="ADDR",
                    help="Endereços de função a desassemblar (hex, ex: 0x100008)")
    ap.add_argument("--default-targets", action="store_true",
                    help="Desassemblar os alvos padrão do projeto")
    ap.add_argument("--summary-only", action="store_true",
                    help="Mostrar só o resumo, sem listagem instrução a instrução")
    ap.add_argument("--max-insns", type=int, default=MAX_INSNS,
                    help=f"Máximo de instruções por função (default {MAX_INSNS})")
    args = ap.parse_args()

    # Auto-detectar ELF
    script_dir = Path(__file__).parent
    candidates = [
        args.elf,
        str(script_dir / "build" / "elf_out" / "SCUS_973.99"),
        str(script_dir / "build" / "elf_out" / "SCUS_973.99.elf"),
    ]
    elf_path = None
    for c in candidates:
        if c and Path(c).exists():
            elf_path = c
            break

    if not elf_path:
        print("ERRO: ELF não encontrado. Extraia com extract_elf_from_gdrive.py ou passe o caminho.")
        print("Candidatos tentados:")
        for c in candidates[1:]:
            print(f"  {c}")
        sys.exit(1)

    print(f"ELF: {elf_path}")
    segments = load_segments(elf_path)
    print(f"Segmentos PT_LOAD: {len(segments)}")

    # Selecionar alvos
    targets = []
    if args.func:
        for addr_str in args.func:
            addr = int(addr_str, 0)
            label = KNOWN_FUNCS.get(addr, f"func_{addr:08x}")
            targets.append((addr, label))
    elif args.default_targets:
        targets = DEFAULT_TARGETS
    else:
        targets = DEFAULT_TARGETS
        print(f"(Modo padrão: {len(targets)} alvos. Use --func 0xADDR para específicos.)\n")

    # Analisar cada alvo
    for func_addr, func_label in targets:
        raw_check = read_vaddr(segments, func_addr, 4)
        if raw_check is None:
            print(f"\n⚠️  0x{func_addr:08X} ({func_label}): NÃO encontrado no ELF")
            continue

        print(f"\nDesassemblando 0x{func_addr:08X}: {func_label} ...")
        disasm   = disassemble_function(segments, func_addr, args.max_insns)
        analysis = analyze_function(disasm)

        if not args.summary_only:
            print_disasm(disasm, func_addr, func_label)

        print_summary(analysis, func_addr)
        suggest_hle_stub(analysis, func_addr)

    print(f"\n{'═'*70}")
    print("  PRÓXIMOS PASSOS:")
    print("  ─────────────────────────────────────────────────────────────────")
    print("  1. Se a função spin-waita em registrador MIPS:")
    print("     → Identificar qual flag na RAM PS2 desbloqueia o branch")
    print("     → Setar essa flag no bios_stub.cpp no momento correto")
    print()
    print("  2. Se a função chama SifGetReg (syscall 0x7B):")
    print("     → Garantir que bios_stub.cpp retorna 0x70000 (SIF pronto)")
    print("     → após YIELD_FIRST chamadas de RotateThreadReadyQueue")
    print()
    print("  3. Salvar análise:")
    print(f"     python3 {Path(__file__).name} --default-targets > /tmp/disasm_gow.txt")
    print()


if __name__ == "__main__":
    main()
