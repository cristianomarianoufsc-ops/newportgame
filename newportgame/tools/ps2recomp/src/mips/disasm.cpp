#include "disasm.h"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstring>

static inline uint32_t BITS(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1);
}
static inline int32_t SIGN_EXT16(uint32_t v) {
    return (int16_t)(v & 0xFFFF);
}

std::string MIPSDisassembler::reg(uint8_t r) {
    return MIPS_REG_NAMES[r & 31];
}

std::string MIPSDisassembler::freg(uint8_t r) {
    return "$f" + std::to_string(r & 31);
}

std::string DecodedInstr::to_string() const {
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << pc << "  "
        << std::setw(8) << std::setfill('0') << raw << "   "
        << std::setfill(' ')   // reset fill before mnemonic
        << std::left << std::setw(10) << mnemonic
        << operands;
    return oss.str();
}

DecodedInstr MIPSDisassembler::disassemble(uint32_t word, uint32_t pc) {
    DecodedInstr d;
    d.raw    = word;
    d.pc     = pc;
    d.type   = InstrType::UNKNOWN;
    d.category = InstrCategory::UNKNOWN;

    d.op    = BITS(word, 31, 26);
    d.rs    = BITS(word, 25, 21);
    d.rt    = BITS(word, 20, 16);
    d.rd    = BITS(word, 15, 11);
    d.shamt = BITS(word, 10,  6);
    d.funct = BITS(word,  5,  0);
    d.imm   = (int16_t)BITS(word, 15, 0);
    d.uimm  = BITS(word, 15, 0);
    d.target = BITS(word, 25, 0);

    MIPSOpcode op = (MIPSOpcode)d.op;

    if (word == 0x00000000) {
        d.mnemonic = "nop";
        d.type     = InstrType::NOP;
        d.category = InstrCategory::NOP;
        return d;
    }

    switch (op) {
    case MIPSOpcode::SPECIAL:
        return decode_special(word, pc);
    case MIPSOpcode::REGIMM:
        return decode_regimm(word, pc);
    case MIPSOpcode::COP0:
        // COP0 — eret, ei, di, mfc0, mtc0 etc. — no-op in host
        d.category = InstrCategory::NOP;
        d.operands = "";
        if (BITS(word, 25, 25)) { // CO=1
            switch (BITS(word, 5, 0)) {
            case 0x18: d.mnemonic = "eret"; break;
            case 0x38: d.mnemonic = "ei";   break;
            case 0x39: d.mnemonic = "di";   break;
            default:   d.mnemonic = "cop0.co"; break;
            }
        } else {
            // MF0/MT0 — real COP0 register moves (Status etc.). Needed:
            // GoW's DI/EI critical sections spin on Status bit 16 (EIE);
            // treating mfc0 as nop makes that loop infinite.
            switch (BITS(word, 25, 21)) {
            case 0x00: d.mnemonic = "mfc0"; break;
            case 0x04: d.mnemonic = "mtc0"; break;
            default:   d.mnemonic = "cop0"; break;
            }
        }
        return d;
    case MIPSOpcode::COP1:
        return decode_cop1(word, pc);
    case MIPSOpcode::CACHE:
        // Cache maintenance — safe NOP in software recompilation
        d.category = InstrCategory::NOP;
        d.mnemonic = "cache";
        d.operands = "";
        return d;
    case MIPSOpcode::COP2:
        // VU0 macro-mode instructions — op=0x12
        // These drive the VU0 coprocessor from the EE main CPU.
        // We decode them as VECTOR (NOP stub) to remove UNHANDLED noise.
        {
            d.category = InstrCategory::VECTOR;
            d.type     = InstrType::R_TYPE;
            // Decode the sub-type enough to name it; actual emulation is NOP.
            uint32_t sub = BITS(word, 25, 21);  // bits 25:21 = co or qmfc2/qmtc2 etc.
            uint32_t fd  = BITS(word, 15, 11);
            uint32_t fs  = BITS(word, 20, 16);
            d.rs = (uint8_t)BITS(word, 25, 21);
            d.rt = (uint8_t)BITS(word, 20, 16);
            d.rd = (uint8_t)BITS(word, 15, 11);
            if (sub == 0x01)       d.mnemonic = "qmfc2";
            else if (sub == 0x05)  d.mnemonic = "qmtc2";
            else if (sub == 0x02)  d.mnemonic = "cfc2";
            else if (sub == 0x06)  d.mnemonic = "ctc2";
            else                   d.mnemonic = "vu0.cop2";
            (void)fd; (void)fs;
            d.operands = "";
        }
        return d;
    case MIPSOpcode::MMI:
        return decode_mmi(word, pc);

    case MIPSOpcode::J:
        d.type = InstrType::J_TYPE;
        d.category = InstrCategory::JUMP;
        d.mnemonic = "j";
        d.branch_target = ((pc + 4) & 0xF0000000) | (d.target << 2);
        { std::ostringstream os; os << "0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;
    case MIPSOpcode::JAL:
        d.type = InstrType::J_TYPE;
        d.category = InstrCategory::JUMP;
        d.mnemonic = "jal";
        d.branch_target = ((pc + 4) & 0xF0000000) | (d.target << 2);
        { std::ostringstream os; os << "0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;

    case MIPSOpcode::BEQ:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = (d.rs == 0 && d.rt == 0) ? "b" : "beq";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os;
          if (d.mnemonic == "b") { os << "0x" << std::hex << d.branch_target; }
          else { os << reg(d.rs) << ", " << reg(d.rt) << ", 0x" << std::hex << d.branch_target; }
          d.operands = os.str(); }
        return d;
    case MIPSOpcode::BNE:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = "bne";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt) << ", 0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;
    case MIPSOpcode::BLEZ:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = "blez";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os; os << reg(d.rs) << ", 0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;
    case MIPSOpcode::BGTZ:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = "bgtz";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os; os << reg(d.rs) << ", 0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;
    case MIPSOpcode::BEQL:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = "beql";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt) << ", 0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;
    case MIPSOpcode::BNEL:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = "bnel";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt) << ", 0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;
    case MIPSOpcode::BLEZL:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = "blezl";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os; os << reg(d.rs) << ", 0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;
    case MIPSOpcode::BGTZL:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::BRANCH;
        d.mnemonic = "bgtzl";
        d.branch_target = pc + 4 + ((int32_t)d.imm << 2);
        { std::ostringstream os; os << reg(d.rs) << ", 0x" << std::hex << d.branch_target; d.operands = os.str(); }
        return d;

    case MIPSOpcode::ADDI:   d.mnemonic = "addi";   d.category = InstrCategory::ALU;   goto i_rsi_rt;
    case MIPSOpcode::ADDIU:  d.mnemonic = "addiu";  d.category = InstrCategory::ALU;   goto i_rsi_rt;
    case MIPSOpcode::SLTI:   d.mnemonic = "slti";   d.category = InstrCategory::ALU;   goto i_rsi_rt;
    case MIPSOpcode::SLTIU:  d.mnemonic = "sltiu";  d.category = InstrCategory::ALU;   goto i_rsi_rt;
    case MIPSOpcode::ANDI:   d.mnemonic = "andi";   d.category = InstrCategory::ALU;   goto i_rsu_rt;
    case MIPSOpcode::ORI:    d.mnemonic = "ori";    d.category = InstrCategory::ALU;   goto i_rsu_rt;
    case MIPSOpcode::XORI:   d.mnemonic = "xori";   d.category = InstrCategory::ALU;   goto i_rsu_rt;
    case MIPSOpcode::DADDI:  d.mnemonic = "daddi";  d.category = InstrCategory::ALU;   goto i_rsi_rt;
    case MIPSOpcode::DADDIU: d.mnemonic = "daddiu"; d.category = InstrCategory::ALU;   goto i_rsi_rt;

    i_rsi_rt:
        d.type = InstrType::I_TYPE;
        { std::ostringstream os; os << reg(d.rt) << ", " << reg(d.rs) << ", " << (int32_t)d.imm; d.operands = os.str(); }
        return d;
    i_rsu_rt:
        d.type = InstrType::I_TYPE;
        { std::ostringstream os; os << reg(d.rt) << ", " << reg(d.rs) << ", 0x" << std::hex << d.uimm; d.operands = os.str(); }
        return d;

    case MIPSOpcode::LUI:
        d.type = InstrType::I_TYPE; d.category = InstrCategory::ALU;
        d.mnemonic = "lui";
        { std::ostringstream os; os << reg(d.rt) << ", 0x" << std::hex << d.uimm; d.operands = os.str(); }
        return d;

    case MIPSOpcode::LB:   d.mnemonic = "lb";   d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LH:   d.mnemonic = "lh";   d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LWL:  d.mnemonic = "lwl";  d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LW:   d.mnemonic = "lw";   d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LBU:  d.mnemonic = "lbu";  d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LHU:  d.mnemonic = "lhu";  d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LWR:  d.mnemonic = "lwr";  d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LWU:  d.mnemonic = "lwu";  d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LD:   d.mnemonic = "ld";   d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LDL:  d.mnemonic = "ldl";  d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LDR:  d.mnemonic = "ldr";  d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LQ:   d.mnemonic = "lq";   d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LWC1: d.mnemonic = "lwc1"; d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::LQC2: d.mnemonic = "lqc2"; d.category = InstrCategory::LOAD; goto i_mem;
    case MIPSOpcode::SB:   d.mnemonic = "sb";   d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SH:   d.mnemonic = "sh";   d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SWL:  d.mnemonic = "swl";  d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SW:   d.mnemonic = "sw";   d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SDL:  d.mnemonic = "sdl";  d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SDR:  d.mnemonic = "sdr";  d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SWR:  d.mnemonic = "swr";  d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SD:   d.mnemonic = "sd";   d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SQ:   d.mnemonic = "sq";   d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SWC1: d.mnemonic = "swc1"; d.category = InstrCategory::STORE; goto i_mem;
    case MIPSOpcode::SQC2: d.mnemonic = "sqc2"; d.category = InstrCategory::STORE; goto i_mem;
    i_mem:
        d.type = InstrType::I_TYPE;
        { std::ostringstream os; os << reg(d.rt) << ", " << (int32_t)d.imm << "(" << reg(d.rs) << ")"; d.operands = os.str(); }
        return d;

    default:
        d.mnemonic = "???";
        { std::ostringstream os; os << "0x" << std::hex << word; d.operands = os.str(); }
        return d;
    }
}

DecodedInstr MIPSDisassembler::decode_special(uint32_t word, uint32_t pc) {
    DecodedInstr d;
    d.raw = word; d.pc = pc;
    d.op    = BITS(word, 31, 26);
    d.rs    = BITS(word, 25, 21);
    d.rt    = BITS(word, 20, 16);
    d.rd    = BITS(word, 15, 11);
    d.shamt = BITS(word, 10,  6);
    d.funct = BITS(word,  5,  0);
    d.imm   = (int16_t)BITS(word, 15, 0);
    d.uimm  = BITS(word, 15, 0);
    d.type  = InstrType::R_TYPE;
    d.category = InstrCategory::ALU;

    auto r3 = [&]{ std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); };
    auto r2shamt = [&](bool use_rd){ std::ostringstream os; os << reg(d.rd) << ", " << reg(use_rd ? d.rt : d.rs) << ", " << (int)d.shamt; d.operands = os.str(); };
    auto r2v = [&]{ std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rt) << ", " << reg(d.rs); d.operands = os.str(); };

    switch ((MIPSSpecial)d.funct) {
    case MIPSSpecial::SLL:   d.mnemonic = "sll";   r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::SRL:   d.mnemonic = "srl";   r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::SRA:   d.mnemonic = "sra";   r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::SLLV:  d.mnemonic = "sllv";  r2v(); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::SRLV:  d.mnemonic = "srlv";  r2v(); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::SRAV:  d.mnemonic = "srav";  r2v(); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::JR:
        d.category = InstrCategory::JUMP;
        d.mnemonic = "jr";
        d.operands = reg(d.rs);
        break;
    case MIPSSpecial::JALR:
        d.category = InstrCategory::JUMP;
        d.mnemonic = "jalr";
        { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rs); d.operands = os.str(); }
        break;
    case MIPSSpecial::MOVZ: d.mnemonic = "movz"; r3(); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::MOVN: d.mnemonic = "movn"; r3(); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::SYSCALL:
        d.type = InstrType::SYSCALL; d.category = InstrCategory::SYSCALL;
        d.mnemonic = "syscall";
        break;
    case MIPSSpecial::SYNC:
        d.mnemonic = "sync";
        d.category = InstrCategory::NOP;
        break;
    case MIPSSpecial::BREAK:
        d.category = InstrCategory::TRAP;
        d.mnemonic = "break";
        break;
    case MIPSSpecial::MFHI: d.mnemonic = "mfhi"; d.operands = reg(d.rd); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::MTHI: d.mnemonic = "mthi"; d.operands = reg(d.rs); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::MFLO: d.mnemonic = "mflo"; d.operands = reg(d.rd); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::MTLO: d.mnemonic = "mtlo"; d.operands = reg(d.rs); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::DSLLV: d.mnemonic = "dsllv"; r2v(); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::DSRLV: d.mnemonic = "dsrlv"; r2v(); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::DSRAV: d.mnemonic = "dsrav"; r2v(); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::MULT:  d.mnemonic = "mult";  d.category = InstrCategory::MULTIPLY; { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); } break;
    case MIPSSpecial::MULTU: d.mnemonic = "multu"; d.category = InstrCategory::MULTIPLY; { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); } break;
    case MIPSSpecial::DIV:   d.mnemonic = "div";   d.category = InstrCategory::DIVIDE;   { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); } break;
    case MIPSSpecial::DIVU:  d.mnemonic = "divu";  d.category = InstrCategory::DIVIDE;   { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); } break;
    case MIPSSpecial::ADD:   d.mnemonic = "add";   r3(); break;
    case MIPSSpecial::ADDU:  d.mnemonic = "addu";  r3(); break;
    case MIPSSpecial::SUB:   d.mnemonic = "sub";   r3(); break;
    case MIPSSpecial::SUBU:  d.mnemonic = "subu";  r3(); break;
    case MIPSSpecial::AND:   d.mnemonic = "and";   r3(); break;
    case MIPSSpecial::OR:    d.mnemonic = (d.rs == 0 || d.rt == 0) ? "move" : "or"; r3(); d.category = (d.rs==0||d.rt==0) ? InstrCategory::MOVE : InstrCategory::ALU; break;
    case MIPSSpecial::XOR:   d.mnemonic = "xor";   r3(); break;
    case MIPSSpecial::NOR:   d.mnemonic = "nor";   r3(); break;
    case MIPSSpecial::SLT:   d.mnemonic = "slt";   r3(); break;
    case MIPSSpecial::SLTU:  d.mnemonic = "sltu";  r3(); break;
    case MIPSSpecial::DADD:  d.mnemonic = "dadd";  r3(); break;
    case MIPSSpecial::DADDU: d.mnemonic = "daddu"; r3(); break;
    case MIPSSpecial::DSUB:  d.mnemonic = "dsub";  r3(); break;
    case MIPSSpecial::DSUBU: d.mnemonic = "dsubu"; r3(); break;
    case MIPSSpecial::DSLL:  d.mnemonic = "dsll";  r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::DSRL:  d.mnemonic = "dsrl";  r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::DSRA:  d.mnemonic = "dsra";  r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::DSLL32: d.mnemonic = "dsll32"; r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::DSRL32: d.mnemonic = "dsrl32"; r2shamt(true); d.category = InstrCategory::SHIFT; break;
    case MIPSSpecial::DSRA32: d.mnemonic = "dsra32"; r2shamt(true); d.category = InstrCategory::SHIFT; break;
    default:
        d.mnemonic = "special?";
        { std::ostringstream os; os << "0x" << std::hex << d.funct; d.operands = os.str(); }
        break;
    }
    return d;
}

DecodedInstr MIPSDisassembler::decode_regimm(uint32_t word, uint32_t pc) {
    DecodedInstr d;
    d.raw = word; d.pc = pc;
    d.op    = BITS(word, 31, 26);
    d.rs    = BITS(word, 25, 21);
    d.rt    = BITS(word, 20, 16);
    d.imm   = (int16_t)BITS(word, 15, 0);
    d.type  = InstrType::I_TYPE;
    d.category = InstrCategory::BRANCH;
    d.branch_target = pc + 4 + ((int32_t)d.imm << 2);

    switch (d.rt) {
    case 0x00: d.mnemonic = "bltz";   break;
    case 0x01: d.mnemonic = "bgez";   break;
    case 0x02: d.mnemonic = "bltzl";  break;
    case 0x03: d.mnemonic = "bgezl";  break;
    case 0x10: d.mnemonic = "bltzal";  break;
    case 0x11: d.mnemonic = "bgezal";  break;
    default:   d.mnemonic = "regimm?"; break;
    }
    std::ostringstream os;
    os << reg(d.rs) << ", 0x" << std::hex << d.branch_target;
    d.operands = os.str();
    return d;
}

DecodedInstr MIPSDisassembler::decode_cop1(uint32_t word, uint32_t pc) {
    DecodedInstr d;
    d.raw = word; d.pc = pc;
    d.rs  = BITS(word, 25, 21); // fmt
    d.rt  = BITS(word, 20, 16);
    d.rd  = BITS(word, 15, 11);
    d.funct = BITS(word, 5, 0);
    d.type = InstrType::R_TYPE;
    d.category = InstrCategory::FLOAT;

    uint8_t fmt = d.rs;
    if (fmt == 0x08) { // BC1
        d.category = InstrCategory::BRANCH;
        d.branch_target = pc + 4 + ((int32_t)(int16_t)BITS(word, 15, 0) << 2);
        d.mnemonic = (d.rt & 1) ? "bc1t" : "bc1f";
        std::ostringstream os; os << "0x" << std::hex << d.branch_target; d.operands = os.str();
        return d;
    }
    if (fmt == 0x00) { // MFC1/CTC1
        d.mnemonic = (BITS(word, 25, 21) == 4) ? "ctc1" : "mfc1";
        std::ostringstream os; os << reg(d.rt) << ", " << freg(d.rd); d.operands = os.str();
        return d;
    }
    if (fmt == 0x04) { d.mnemonic = "mtc1"; std::ostringstream os; os << reg(d.rt) << ", " << freg(d.rd); d.operands = os.str(); return d; }

    // EE FPU opcode table — funct 0-63
    // PS2-specific: adda(0x18), suba(0x19), mula(0x1a), madd(0x1c), msub(0x1d),
    //               madda(0x1e), msuba(0x1f), max(0x28), min(0x29)
    // MIPS compare: c.f..c.ngt (0x30-0x3f)
    static const char* op_names[64] = {
        /* 0x00-0x07 */ "add","sub","mul","div","sqrt","abs","mov","neg",
        /* 0x08-0x0f */ "?","?","?","?","round.w","trunc.w","ceil.w","floor.w",
        /* 0x10-0x17 */ "?","?","?","?","?","?","?","?",
        /* 0x18-0x1f */ "adda","suba","mula","?","madd","msub","madda","msuba",
        /* 0x20-0x27 */ "cvt.s","cvt.d","?","?","cvt.w","cvt.l","?","?",
        /* 0x28-0x2f */ "max","min","?","?","?","?","?","?",
        /* 0x30-0x37 */ "c.f","c.un","c.eq","c.ueq","c.olt","c.ult","c.ole","c.ule",
        /* 0x38-0x3f */ "c.sf","c.ngle","c.seq","c.ngl","c.lt","c.nge","c.le","c.ngt"
    };
    if (op_names[d.funct & 0x3f][0] != '?')
        d.mnemonic = std::string(op_names[d.funct & 0x3f]) + ".s";
    else
        d.mnemonic = "cop1?";
    std::ostringstream os; os << freg(d.rd) << ", " << freg(d.rs) << ", " << freg(d.rt); d.operands = os.str();
    return d;
}

// EE MMI sub-opcode tables (EE User's Manual, Appendix A)
// MMI0 sub-ops (funct=0x08, sub in bits [5:0] using shamt+funct overlap)
static const char* mmi0_names[32] = {
    "paddw","psubw","pcgtw","pmaxw",
    "paddh","psubh","pcgth","pmaxh",
    "paddb","psubb","pcgtb","?",
    "?","?","?","?",
    "paddsw","psubsw","pextlw","ppacw",
    "paddsh","psubsh","pextlh","ppach",
    "paddsb","psubsb","pextlb","ppacb",
    "?","?","pext5","ppac5"
};
// MMI1 sub-ops (funct=0x28)
static const char* mmi1_names[32] = {
    "?","pabsw","pceqw","pminw",
    "padsbh","pabsh","pceqh","pminh",
    "?","?","pceqb","?",
    "?","?","?","?",
    "padduw","psubuw","pextuw","?",
    "padduh","psubuh","pextuh","?",
    "paddub","psubub","pextub","qfsrv",
    "?","?","?","?"
};
// MMI2 sub-ops (funct=0x09)
static const char* mmi2_names[32] = {
    "pmaddw","?","psllvw","psrlvw",
    "pmsubw","?","?","?",
    "pmfhi","pmflo","pinth","?",
    "pmultw","pdivw","pcpyld","?",
    "pmaddh","phmadh","pand","pxor",
    "pmsubh","phmsbh","?","?",
    "?","?","pinteh","?",
    "pmulth","pdivbw","pcpyud","?"
};
// MMI3 sub-ops (funct=0x29)
static const char* mmi3_names[32] = {
    "pmadduw","?","?","psravw",
    "?","?","?","?",
    "pmthi","pmtlo","pintoh","?",
    "pmultuw","pdivuw","pcpyh","?",
    "?","phmadh_?","por","pnor",
    "?","phmsbh_?","?","?",
    "?","?","?","?",
    "?","?","pcpyh_?","?"
};

DecodedInstr MIPSDisassembler::decode_mmi(uint32_t word, uint32_t pc) {
    DecodedInstr d;
    d.raw = word; d.pc = pc;
    d.type = InstrType::R_TYPE;
    d.category = InstrCategory::VECTOR;

    d.rs    = BITS(word, 25, 21);
    d.rt    = BITS(word, 20, 16);
    d.rd    = BITS(word, 15, 11);
    d.shamt = BITS(word, 10,  6);
    d.funct = BITS(word,  5,  0);

    // Helper to format 3-reg operands
    auto fmt3 = [&]{ std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); };

    switch (d.funct) {
    // --- Scalar MMI ops ---
    case 0x00: d.mnemonic = "madd";  fmt3(); break;
    case 0x01: d.mnemonic = "maddu"; fmt3(); break;
    case 0x04: d.mnemonic = "plzcw";
               { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rs); d.operands = os.str(); } break;
    case 0x10: d.mnemonic = "mfhi1"; d.operands = reg(d.rd); break;
    case 0x11: d.mnemonic = "mthi1"; d.operands = reg(d.rs); break;
    case 0x12: d.mnemonic = "mflo1"; d.operands = reg(d.rd); break;
    case 0x13: d.mnemonic = "mtlo1"; d.operands = reg(d.rs); break;
    case 0x18: d.mnemonic = "mult1"; fmt3(); break;
    case 0x19: d.mnemonic = "multu1";fmt3(); break;
    case 0x1A: d.mnemonic = "div1";  { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); } break;
    case 0x1B: d.mnemonic = "divu1"; { std::ostringstream os; os << reg(d.rs) << ", " << reg(d.rt); d.operands = os.str(); } break;
    case 0x1C: d.mnemonic = "madd1"; fmt3(); break;
    case 0x1D: d.mnemonic = "maddu1";fmt3(); break;
    case 0x30: { // PMFHL — sub-op in shamt
        const char* pmfhl_names[] = {"pmfhl.lw","pmfhl.uw","pmfhl.slw","pmfhl.lh","pmfhl.sh"};
        d.mnemonic = (d.shamt < 5) ? pmfhl_names[d.shamt] : "pmfhl?";
        d.operands = reg(d.rd); break;
    }
    case 0x31: d.mnemonic = "pmthl.lw"; d.operands = reg(d.rs); break;
    case 0x34: d.mnemonic = "psllh";
               { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rt) << ", " << (int)d.shamt; d.operands = os.str(); } break;
    case 0x36: d.mnemonic = "psrlh";
               { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rt) << ", " << (int)d.shamt; d.operands = os.str(); } break;
    case 0x37: d.mnemonic = "psrah";
               { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rt) << ", " << (int)d.shamt; d.operands = os.str(); } break;
    case 0x3C: d.mnemonic = "psllw";
               { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rt) << ", " << (int)d.shamt; d.operands = os.str(); } break;
    case 0x3E: d.mnemonic = "psrlw";
               { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rt) << ", " << (int)d.shamt; d.operands = os.str(); } break;
    case 0x3F: d.mnemonic = "psraw";
               { std::ostringstream os; os << reg(d.rd) << ", " << reg(d.rt) << ", " << (int)d.shamt; d.operands = os.str(); } break;

    // --- MMI0 parallel ops (sub-op in bits [10:6]) ---
    case 0x08: {
        uint8_t sub = d.shamt & 0x1F;
        d.mnemonic = (sub < 32 && mmi0_names[sub][0] != '?') ? mmi0_names[sub] : ("mmi0." + std::to_string(sub));
        fmt3(); break;
    }
    // --- MMI2 parallel ops ---
    case 0x09: {
        uint8_t sub = d.shamt & 0x1F;
        d.mnemonic = (sub < 32 && mmi2_names[sub][0] != '?') ? mmi2_names[sub] : ("mmi2." + std::to_string(sub));
        fmt3(); break;
    }
    // --- MMI1 parallel ops (sub-op in bits [10:6]) ---
    case 0x28: {
        uint8_t sub = d.shamt & 0x1F;
        d.mnemonic = (sub < 32 && mmi1_names[sub][0] != '?') ? mmi1_names[sub] : ("mmi1." + std::to_string(sub));
        fmt3(); break;
    }
    // --- MMI3 parallel ops ---
    case 0x29: {
        uint8_t sub = d.shamt & 0x1F;
        d.mnemonic = (sub < 32 && mmi3_names[sub][0] != '?') ? mmi3_names[sub] : ("mmi3." + std::to_string(sub));
        fmt3(); break;
    }
    default:
        d.mnemonic = "mmi?";
        { std::ostringstream os; os << "funct=0x" << std::hex << (int)d.funct; d.operands = os.str(); }
        break;
    }
    return d;
}

std::vector<DecodedInstr> MIPSDisassembler::disassemble_block(
    const uint8_t* data, size_t size, uint32_t base_pc)
{
    std::vector<DecodedInstr> result;
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t word;
        memcpy(&word, data + i, 4);
        result.push_back(disassemble(word, base_pc + (uint32_t)i));
    }
    return result;
}

void MIPSDisassembler::print_block(
    const uint8_t* data, size_t size, uint32_t base_pc, size_t max_instrs)
{
    size_t count = 0;
    for (size_t i = 0; i + 4 <= size && count < max_instrs; i += 4, ++count) {
        uint32_t word;
        memcpy(&word, data + i, 4);
        auto d = disassemble(word, base_pc + (uint32_t)i);
        std::cout << d.to_string() << "\n";
    }
}
