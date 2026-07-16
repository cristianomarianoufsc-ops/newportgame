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
    case MIPSOpcode::COP1:
        return decode_cop1(word, pc);
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
    case MIPSSpecial::SLL:   d.mnemonic = "sll";   r2shamt(true); break;
    case MIPSSpecial::SRL:   d.mnemonic = "srl";   r2shamt(true); break;
    case MIPSSpecial::SRA:   d.mnemonic = "sra";   r2shamt(true); break;
    case MIPSSpecial::SLLV:  d.mnemonic = "sllv";  r2v(); break;
    case MIPSSpecial::SRLV:  d.mnemonic = "srlv";  r2v(); break;
    case MIPSSpecial::SRAV:  d.mnemonic = "srav";  r2v(); break;
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
    case MIPSSpecial::MOVZ: d.mnemonic = "movz"; r3(); break;
    case MIPSSpecial::MOVN: d.mnemonic = "movn"; r3(); break;
    case MIPSSpecial::SYSCALL:
        d.type = InstrType::SYSCALL; d.category = InstrCategory::SYSCALL;
        d.mnemonic = "syscall";
        break;
    case MIPSSpecial::BREAK:
        d.category = InstrCategory::TRAP;
        d.mnemonic = "break";
        break;
    case MIPSSpecial::MFHI: d.mnemonic = "mfhi"; d.operands = reg(d.rd); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::MTHI: d.mnemonic = "mthi"; d.operands = reg(d.rs); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::MFLO: d.mnemonic = "mflo"; d.operands = reg(d.rd); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::MTLO: d.mnemonic = "mtlo"; d.operands = reg(d.rs); d.category = InstrCategory::MOVE; break;
    case MIPSSpecial::DSLLV: d.mnemonic = "dsllv"; r2v(); break;
    case MIPSSpecial::DSRLV: d.mnemonic = "dsrlv"; r2v(); break;
    case MIPSSpecial::DSRAV: d.mnemonic = "dsrav"; r2v(); break;
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

    const char* op_names[] = {"add","sub","mul","div","sqrt","abs","mov","neg",
                               "?","?","?","?","round.w","trunc.w","ceil.w","floor.w",
                               "?","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?",
                               "cvt.s","cvt.d","?","?","cvt.w","cvt.l"};
    if (d.funct < 36)
        d.mnemonic = std::string(op_names[d.funct]) + ".s";
    else
        d.mnemonic = "cop1?";
    std::ostringstream os; os << freg(d.rd) << ", " << freg(d.rs) << ", " << freg(d.rt); d.operands = os.str();
    return d;
}

DecodedInstr MIPSDisassembler::decode_mmi(uint32_t word, uint32_t pc) {
    DecodedInstr d;
    d.raw = word; d.pc = pc;
    d.type = InstrType::R_TYPE;
    d.category = InstrCategory::VECTOR;
    d.funct = BITS(word, 5, 0);
    // MMI has sub-opcodes; just label them for now
    d.mnemonic = "mmi." + std::to_string(d.funct);
    std::ostringstream os;
    os << "0x" << std::hex << word;
    d.operands = os.str();
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
