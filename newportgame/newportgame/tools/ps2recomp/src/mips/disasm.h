#pragma once
#include <cstdint>
#include <string>
#include <vector>

// MIPS R5900 (Emotion Engine) Disassembler
// Supports: MIPS III baseline + EE extensions (MMI, COP1/FPU, COP2/VU0)

// Instruction formats
//   R-type: [ op:6 | rs:5 | rt:5 | rd:5 | shamt:5 | funct:6 ]
//   I-type: [ op:6 | rs:5 | rt:5 | imm:16 ]
//   J-type: [ op:6 | target:26 ]

enum class MIPSOpcode : uint32_t {
    SPECIAL  = 0x00,
    REGIMM   = 0x01,
    J        = 0x02,
    JAL      = 0x03,
    BEQ      = 0x04,
    BNE      = 0x05,
    BLEZ     = 0x06,
    BGTZ     = 0x07,
    ADDI     = 0x08,
    ADDIU    = 0x09,
    SLTI     = 0x0A,
    SLTIU    = 0x0B,
    ANDI     = 0x0C,
    ORI      = 0x0D,
    XORI     = 0x0E,
    LUI      = 0x0F,
    COP0     = 0x10,
    COP1     = 0x11,
    COP2     = 0x12,
    BEQL     = 0x14,
    BNEL     = 0x15,
    BLEZL    = 0x16,
    BGTZL    = 0x17,
    DADDI    = 0x18,
    DADDIU   = 0x19,
    LDL      = 0x1A,
    LDR      = 0x1B,
    MMI      = 0x1C,   // EE-specific MMI instructions
    LQ       = 0x1E,   // EE-specific: load quadword
    SQ       = 0x1F,   // EE-specific: store quadword
    LB       = 0x20,
    LH       = 0x21,
    LWL      = 0x22,
    LW       = 0x23,
    LBU      = 0x24,
    LHU      = 0x25,
    LWR      = 0x26,
    LWU      = 0x27,
    SB       = 0x28,
    SH       = 0x29,
    SWL      = 0x2A,
    SW       = 0x2B,
    SDL      = 0x2C,
    SDR      = 0x2D,
    SWR      = 0x2E,
    CACHE    = 0x2F,
    LWC1     = 0x31,
    LQC2     = 0x36,   // EE-specific: load quad to VU0
    LD       = 0x37,
    SWC1     = 0x39,
    SQC2     = 0x3E,   // EE-specific: store quad from VU0
    SD       = 0x3F,
};

enum class MIPSSpecial : uint32_t {
    SLL      = 0x00,
    SRL      = 0x02,
    SRA      = 0x03,
    SLLV     = 0x04,
    SRLV     = 0x06,
    SRAV     = 0x07,
    JR       = 0x08,
    JALR     = 0x09,
    MOVZ     = 0x0A,
    MOVN     = 0x0B,
    SYSCALL  = 0x0C,
    BREAK    = 0x0D,
    SYNC     = 0x0F,
    MFHI     = 0x10,
    MTHI     = 0x11,
    MFLO     = 0x12,
    MTLO     = 0x13,
    DSLLV    = 0x14,
    DSRLV    = 0x16,
    DSRAV    = 0x17,
    MULT     = 0x18,
    MULTU    = 0x19,
    DIV      = 0x1A,
    DIVU     = 0x1B,
    ADD      = 0x20,
    ADDU     = 0x21,
    SUB      = 0x22,
    SUBU     = 0x23,
    AND      = 0x24,
    OR       = 0x25,
    XOR      = 0x26,
    NOR      = 0x27,
    SLT      = 0x2A,
    SLTU     = 0x2B,
    DADD     = 0x2C,
    DADDU    = 0x2D,
    DSUB     = 0x2E,
    DSUBU    = 0x2F,
    TGE      = 0x30,
    TGEU     = 0x31,
    TLT      = 0x32,
    TLTU     = 0x33,
    TEQ      = 0x34,
    TNE      = 0x36,
    DSLL     = 0x38,
    DSRL     = 0x3A,
    DSRA     = 0x3B,
    DSLL32   = 0x3C,
    DSRL32   = 0x3E,
    DSRA32   = 0x3F,
};

static const char* const MIPS_REG_NAMES[32] = {
    "$zero","$at","$v0","$v1","$a0","$a1","$a2","$a3",
    "$t0","$t1","$t2","$t3","$t4","$t5","$t6","$t7",
    "$s0","$s1","$s2","$s3","$s4","$s5","$s6","$s7",
    "$t8","$t9","$k0","$k1","$gp","$sp","$fp","$ra"
};

enum class InstrType {
    UNKNOWN,
    R_TYPE,
    I_TYPE,
    J_TYPE,
    // Special EE
    SYSCALL,
    BREAK,
    NOP,
};

enum class InstrCategory {
    UNKNOWN,
    ALU,
    LOAD,
    STORE,
    BRANCH,
    JUMP,
    SYSCALL,
    MULTIPLY,
    DIVIDE,
    FLOAT,
    VECTOR,   // EE VU0/MMI
    MOVE,
    SHIFT,
    TRAP,
    NOP,
};

struct DecodedInstr {
    uint32_t        raw;
    uint32_t        pc;
    InstrType       type;
    InstrCategory   category;
    std::string     mnemonic;
    std::string     operands;

    // Decoded fields
    uint8_t  op;     // [31:26]
    uint8_t  rs;     // [25:21]
    uint8_t  rt;     // [20:16]
    uint8_t  rd;     // [15:11]
    uint8_t  shamt;  // [10:6]
    uint8_t  funct;  // [5:0]
    int16_t  imm;    // [15:0] signed
    uint32_t uimm;   // [15:0] unsigned
    uint32_t target; // [25:0]

    bool is_branch() const { return category == InstrCategory::BRANCH; }
    bool is_jump()   const { return category == InstrCategory::JUMP; }
    bool is_load()   const { return category == InstrCategory::LOAD; }
    bool is_store()  const { return category == InstrCategory::STORE; }

    // Resolved branch/jump target (0 if not applicable)
    uint32_t branch_target = 0;

    std::string to_string() const;
};

class MIPSDisassembler {
public:
    // Disassemble a single instruction at the given PC
    static DecodedInstr disassemble(uint32_t word, uint32_t pc = 0);

    // Disassemble a block of instructions
    static std::vector<DecodedInstr> disassemble_block(
        const uint8_t* data, size_t size, uint32_t base_pc);

    // Disassemble and print a range
    static void print_block(
        const uint8_t* data, size_t size, uint32_t base_pc,
        size_t max_instrs = SIZE_MAX);

private:
    static DecodedInstr decode_special(uint32_t word, uint32_t pc);
    static DecodedInstr decode_regimm(uint32_t word, uint32_t pc);
    static DecodedInstr decode_cop1(uint32_t word, uint32_t pc);
    static DecodedInstr decode_mmi(uint32_t word, uint32_t pc);
    static std::string reg(uint8_t r);
    static std::string freg(uint8_t r);
};
