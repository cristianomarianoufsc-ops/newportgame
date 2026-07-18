#include "recompiler.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <set>
#include <queue>
#include <algorithm>

Recompiler::Recompiler(const ELFFile& elf, const RecompOptions& opts)
    : m_elf(elf), m_opts(opts) {}

void Recompiler::analyze() {
    std::cout << "[RECOMP] Starting function discovery from entry 0x"
              << std::hex << m_elf.entry_point << std::dec << "\n";

    // Use iterative BFS — avoid stack overflow on deep call graphs
    std::queue<std::pair<uint32_t, std::string>> work;
    work.push({m_elf.entry_point, "ps2_entry"});

    // Seed from symbol table (named functions)
    for (auto& sym : m_elf.symbols) {
        if ((sym.type & 0xf) == 2 && sym.value != 0 && !sym.name.empty()) {
            work.push({sym.value, sym.name});
        }
    }

    static constexpr size_t MAX_FUNCTIONS = 50000;

    while (!work.empty() && m_functions.size() < MAX_FUNCTIONS) {
        auto [addr, name] = work.front();
        work.pop();

        if (m_functions.count(addr)) continue;
        if (!m_elf.vaddr_to_offset(addr)) continue;

        discover_function(addr, name);

        // Enqueue callees
        auto it = m_functions.find(addr);
        if (it != m_functions.end()) {
            for (uint32_t target : it->second.call_targets) {
                if (!m_functions.count(target)) {
                    work.push({target, ""});
                }
            }
        }
    }

    std::cout << "[RECOMP] Discovered " << m_functions.size() << " functions\n";
}

void Recompiler::discover_function(uint32_t addr, const std::string& name) {
    if (m_functions.count(addr)) return;
    if (!m_elf.vaddr_to_offset(addr)) return;

    Function func;
    func.start_addr = addr;
    {
        std::ostringstream os;
        os << std::hex << addr;
        func.name = name.empty() ? ("func_" + os.str()) : name;
    }

    uint32_t end = find_function_end(addr);
    func.end_addr = end;

    uint32_t pc = addr;
    bool hit_jr_ra = false;

    while (pc <= end) {
        auto word_opt = m_elf.read32(pc);
        if (!word_opt) break;

        auto instr = MIPSDisassembler::disassemble(*word_opt, pc);
        func.instructions.push_back(instr);

        if (instr.mnemonic == "jal" && instr.branch_target != 0)
            func.call_targets.push_back(instr.branch_target);

        if ((instr.is_branch() || instr.is_jump()) && instr.branch_target != 0)
            func.jump_targets.push_back(instr.branch_target);

        if (instr.mnemonic == "jr" && instr.rs == 31) {
            hit_jr_ra = true;
        } else if (hit_jr_ra) {
            pc += 4;
            break;
        }
        pc += 4;
    }

    m_functions[addr] = std::move(func);
}

uint32_t Recompiler::find_function_end(uint32_t start_addr) {
    uint32_t pc = start_addr;
    uint32_t max_scan = 0x4000; // 16KB max function size

    for (uint32_t i = 0; i < max_scan; i += 4) {
        auto word_opt = m_elf.read32(pc + i);
        if (!word_opt) return pc + i;

        auto instr = MIPSDisassembler::disassemble(*word_opt, pc + i);
        if (instr.mnemonic == "jr" && instr.rs == 31) {
            return pc + i + 4; // Include delay slot
        }
    }
    return pc + max_scan;
}

std::string Recompiler::symbol_name(uint32_t addr) const {
    for (auto& sym : m_elf.symbols) {
        if (sym.value == addr && !sym.name.empty())
            return sym.name;
    }
    std::ostringstream os;
    os << "func_" << std::hex << addr;
    return os.str();
}

std::optional<uint32_t> Recompiler::fetch_word(uint32_t vaddr) const {
    return m_elf.read32(vaddr);
}

// --- C code emission ---

std::string Recompiler::emit_runtime_header() const {
    return R"(// ps2recomp generated output — DO NOT EDIT
// Compile with: gcc -O2 -o game this_file.c -lm
// Requires: ps2_runtime.h (memory, GS stub, IOP stub)

#include <cmath>
#include <stdint.h>
#include <string.h>

// -----------------------------------------------------------------------
// Register file
// -----------------------------------------------------------------------
typedef struct {
    uint64_t r[32];   // GPRs (64-bit EE registers; upper 64 bits of 128-bit GPR ignored here)
    uint64_t hi, lo;  // HI/LO multiply result
    uint64_t hi1, lo1;
    uint32_t pc;
    uint32_t sa;      // Shift amount register
    // FPU
    float    f[32];
    uint32_t fcr31;   // FPU control register
} PS2Regs;

// -----------------------------------------------------------------------
// Memory
// -----------------------------------------------------------------------
// PS2 main RAM: 32 MB at 0x00000000
// Scratchpad:    16 KB at 0x70000000
static uint8_t ps2_ram[32 * 1024 * 1024];
static uint8_t ps2_spr[16 * 1024];

static inline uint8_t* ps2_mem_ptr(uint32_t addr) {
    addr &= 0x1FFFFFFF; // Strip cache/kseg bits
    if (addr < sizeof(ps2_ram)) return ps2_ram + addr;
    if (addr >= 0x70000000 && addr < 0x70004000) return ps2_spr + (addr - 0x70000000);
    return NULL; // I/O or unmapped
}

static inline uint32_t mem_read32(uint32_t addr)  { uint32_t v=0; uint8_t*p=ps2_mem_ptr(addr); if(p) memcpy(&v,p,4); return v; }
static inline uint16_t mem_read16(uint32_t addr)  { uint16_t v=0; uint8_t*p=ps2_mem_ptr(addr); if(p) memcpy(&v,p,2); return v; }
static inline uint8_t  mem_read8 (uint32_t addr)  { uint8_t*p=ps2_mem_ptr(addr); return p?*p:0; }
static inline void mem_write32(uint32_t addr, uint32_t v)  { uint8_t*p=ps2_mem_ptr(addr); if(p) memcpy(p,&v,4); }
static inline void mem_write16(uint32_t addr, uint16_t v)  { uint8_t*p=ps2_mem_ptr(addr); if(p) memcpy(p,&v,2); }
static inline void mem_write8 (uint32_t addr, uint8_t v)   { uint8_t*p=ps2_mem_ptr(addr); if(p) *p=v; }

// -----------------------------------------------------------------------
// GS stub (Graphics Synthesizer -> OpenGL)
// -----------------------------------------------------------------------
void gs_write_reg(uint64_t reg, uint64_t value);  // Defined in gs_stub.c

// -----------------------------------------------------------------------
// Syscall stub
// -----------------------------------------------------------------------
void ps2_syscall(PS2Regs* regs, uint32_t code);   // Defined in bios_stub.c

)";
}

std::string Recompiler::emit_instruction(const DecodedInstr& instr,
                                          const Function& func) const
{
    std::ostringstream out;

    if (m_opts.emit_comments) {
        out << "    // " << instr.to_string() << "\n";
    }
    if (m_opts.emit_pc_tracking) {
        out << "    regs->pc = 0x" << std::hex << instr.pc << std::dec << "u;\n";
    }

    auto R  = [](uint8_t r) -> std::string { return "regs->r[" + std::to_string(r) + "]"; };
    auto F  = [](uint8_t r) -> std::string { return "regs->f[" + std::to_string(r) + "]"; };
    auto SE = [](int16_t v) -> std::string { return std::to_string((int32_t)v); };
    auto HEX = [](uint32_t v) -> std::string { std::ostringstream os; os << "0x" << std::hex << v << "u"; return os.str(); };

    switch (instr.category) {
    case InstrCategory::NOP:
        out << "    /* nop */\n";
        break;
    case InstrCategory::ALU: {
        // ADDIU $rt, $rs, imm  ->  r[rt] = (int32_t)(r[rs] + imm)
        if (instr.mnemonic == "addiu")
            out << "    " << R(instr.rt) << " = (uint32_t)((int32_t)" << R(instr.rs) << " + " << SE(instr.imm) << ");\n";
        else if (instr.mnemonic == "addi")
            out << "    " << R(instr.rt) << " = (uint32_t)((int32_t)" << R(instr.rs) << " + " << SE(instr.imm) << ");\n";
        else if (instr.mnemonic == "addu")
            out << "    " << R(instr.rd) << " = (uint32_t)(" << R(instr.rs) << " + " << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "add")
            out << "    " << R(instr.rd) << " = (uint32_t)(" << R(instr.rs) << " + " << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "subu")
            out << "    " << R(instr.rd) << " = (uint32_t)(" << R(instr.rs) << " - " << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "sub")
            out << "    " << R(instr.rd) << " = (uint32_t)(" << R(instr.rs) << " - " << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "and")
            out << "    " << R(instr.rd) << " = " << R(instr.rs) << " & " << R(instr.rt) << ";\n";
        else if (instr.mnemonic == "or")
            out << "    " << R(instr.rd) << " = " << R(instr.rs) << " | " << R(instr.rt) << ";\n";
        else if (instr.mnemonic == "xor")
            out << "    " << R(instr.rd) << " = " << R(instr.rs) << " ^ " << R(instr.rt) << ";\n";
        else if (instr.mnemonic == "nor")
            out << "    " << R(instr.rd) << " = ~(" << R(instr.rs) << " | " << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "andi")
            out << "    " << R(instr.rt) << " = " << R(instr.rs) << " & " << HEX(instr.uimm) << ";\n";
        else if (instr.mnemonic == "ori")
            out << "    " << R(instr.rt) << " = " << R(instr.rs) << " | " << HEX(instr.uimm) << ";\n";
        else if (instr.mnemonic == "xori")
            out << "    " << R(instr.rt) << " = " << R(instr.rs) << " ^ " << HEX(instr.uimm) << ";\n";
        else if (instr.mnemonic == "lui")
            out << "    " << R(instr.rt) << " = " << HEX((uint32_t)instr.uimm << 16) << ";\n";
        else if (instr.mnemonic == "slt")
            out << "    " << R(instr.rd) << " = ((int32_t)" << R(instr.rs) << " < (int32_t)" << R(instr.rt) << ") ? 1 : 0;\n";
        else if (instr.mnemonic == "sltu")
            out << "    " << R(instr.rd) << " = ((uint32_t)" << R(instr.rs) << " < (uint32_t)" << R(instr.rt) << ") ? 1 : 0;\n";
        else if (instr.mnemonic == "slti")
            out << "    " << R(instr.rt) << " = ((int32_t)" << R(instr.rs) << " < " << SE(instr.imm) << ") ? 1 : 0;\n";
        else if (instr.mnemonic == "sltiu")
            out << "    " << R(instr.rt) << " = ((uint32_t)" << R(instr.rs) << " < (uint32_t)" << SE(instr.imm) << ") ? 1 : 0;\n";
        // 64-bit ALU (EE MIPS III)
        else if (instr.mnemonic == "daddu")
            out << "    " << R(instr.rd) << " = " << R(instr.rs) << " + " << R(instr.rt) << ";\n";
        else if (instr.mnemonic == "dsubu" || instr.mnemonic == "dsub")
            out << "    " << R(instr.rd) << " = " << R(instr.rs) << " - " << R(instr.rt) << ";\n";
        else if (instr.mnemonic == "daddi" || instr.mnemonic == "daddiu")
            out << "    " << R(instr.rt) << " = " << R(instr.rs) << " + (int64_t)(int16_t)(" << SE(instr.imm) << ");\n";
        // Conditional moves
        else if (instr.mnemonic == "movz")
            out << "    if (" << R(instr.rt) << " == 0) " << R(instr.rd) << " = " << R(instr.rs) << ";\n";
        else if (instr.mnemonic == "movn")
            out << "    if (" << R(instr.rt) << " != 0) " << R(instr.rd) << " = " << R(instr.rs) << ";\n";
        // 64-bit variable shifts (SPECIAL encoding, end up as ALU)
        else if (instr.mnemonic == "dsllv")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " << (" << R(instr.rs) << " & 63u);\n";
        else if (instr.mnemonic == "dsrlv")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " >> (" << R(instr.rs) << " & 63u);\n";
        else if (instr.mnemonic == "dsrav")
            out << "    " << R(instr.rd) << " = (uint64_t)((int64_t)" << R(instr.rt) << " >> (" << R(instr.rs) << " & 63u));\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    }
    case InstrCategory::SHIFT:
        if (instr.mnemonic == "sll")
            out << "    " << R(instr.rd) << " = (uint32_t)((uint32_t)" << R(instr.rt) << " << " << (int)instr.shamt << ");\n";
        else if (instr.mnemonic == "srl")
            out << "    " << R(instr.rd) << " = (uint32_t)((uint32_t)" << R(instr.rt) << " >> " << (int)instr.shamt << ");\n";
        else if (instr.mnemonic == "sra")
            out << "    " << R(instr.rd) << " = (uint32_t)((int32_t)" << R(instr.rt) << " >> " << (int)instr.shamt << ");\n";
        else if (instr.mnemonic == "sllv")
            out << "    " << R(instr.rd) << " = (uint32_t)((uint32_t)" << R(instr.rt) << " << (" << R(instr.rs) << " & 31));\n";
        else if (instr.mnemonic == "srlv")
            out << "    " << R(instr.rd) << " = (uint32_t)((uint32_t)" << R(instr.rt) << " >> (" << R(instr.rs) << " & 31));\n";
        else if (instr.mnemonic == "srav")
            out << "    " << R(instr.rd) << " = (uint32_t)((int32_t)" << R(instr.rt) << " >> (" << R(instr.rs) << " & 31));\n";
        // 64-bit immediate shifts
        else if (instr.mnemonic == "dsll")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " << " << (int)instr.shamt << "u;\n";
        else if (instr.mnemonic == "dsrl")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " >> " << (int)instr.shamt << "u;\n";
        else if (instr.mnemonic == "dsra")
            out << "    " << R(instr.rd) << " = (uint64_t)((int64_t)" << R(instr.rt) << " >> " << (int)instr.shamt << ");\n";
        else if (instr.mnemonic == "dsll32")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " << " << (int)(instr.shamt + 32) << "u;\n";
        else if (instr.mnemonic == "dsrl32")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " >> " << (int)(instr.shamt + 32) << "u;\n";
        else if (instr.mnemonic == "dsra32")
            out << "    " << R(instr.rd) << " = (uint64_t)((int64_t)" << R(instr.rt) << " >> " << (int)(instr.shamt + 32) << ");\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::LOAD:
        if (instr.mnemonic == "lw")
            out << "    " << R(instr.rt) << " = mem_read32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lh")
            out << "    " << R(instr.rt) << " = (int32_t)(int16_t)mem_read16((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lhu")
            out << "    " << R(instr.rt) << " = mem_read16((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lb")
            out << "    " << R(instr.rt) << " = (int32_t)(int8_t)mem_read8((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lbu")
            out << "    " << R(instr.rt) << " = mem_read8((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        // 64-bit load (ld)
        else if (instr.mnemonic == "ld")
            out << "    " << R(instr.rt) << " = mem_read64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        // FPU load (lwc1: ft=rt field, rs=base)
        else if (instr.mnemonic == "lwc1")
            out << "    { uint32_t _fv = mem_read32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << ")); memcpy(&regs->f[" << (int)instr.rt << "], &_fv, 4); }\n";
        // 128-bit load (lq) — load lower 64 bits, upper 64 ignored
        else if (instr.mnemonic == "lq")
            out << "    " << R(instr.rt) << " = mem_read64((uint32_t)((" << R(instr.rs) << " + " << SE(instr.imm) << ") & ~15u));\n";
        // load word unsigned (zero-extend)
        else if (instr.mnemonic == "lwu")
            out << "    " << R(instr.rt) << " = (uint64_t)(uint32_t)mem_read32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::STORE:
        if (instr.mnemonic == "sw")
            out << "    mem_write32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), (uint32_t)" << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "sh")
            out << "    mem_write16((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), (uint16_t)" << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "sb")
            out << "    mem_write8((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), (uint8_t)" << R(instr.rt) << ");\n";
        // 64-bit store (sd)
        else if (instr.mnemonic == "sd")
            out << "    mem_write64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), " << R(instr.rt) << ");\n";
        // FPU store (swc1: ft=rt field)
        else if (instr.mnemonic == "swc1")
            out << "    { uint32_t _fv; memcpy(&_fv, &regs->f[" << (int)instr.rt << "], 4); mem_write32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), _fv); }\n";
        // 128-bit store (sq) — store lower 64 bits
        else if (instr.mnemonic == "sq")
            out << "    mem_write64((uint32_t)((" << R(instr.rs) << " + " << SE(instr.imm) << ") & ~15u), " << R(instr.rt) << ");\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::MULTIPLY:
        if (instr.mnemonic == "mult")
            out << "    { int64_t r = (int64_t)(int32_t)" << R(instr.rs) << " * (int64_t)(int32_t)" << R(instr.rt) << "; regs->lo = (uint32_t)r; regs->hi = (uint32_t)(r>>32); }\n";
        else if (instr.mnemonic == "multu")
            out << "    { uint64_t r = (uint64_t)(uint32_t)" << R(instr.rs) << " * (uint64_t)(uint32_t)" << R(instr.rt) << "; regs->lo = (uint32_t)r; regs->hi = (uint32_t)(r>>32); }\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::DIVIDE:
        if (instr.mnemonic == "div")
            out << "    if (" << R(instr.rt) << ") { regs->lo = (uint32_t)((int32_t)" << R(instr.rs) << " / (int32_t)" << R(instr.rt) << "); regs->hi = (uint32_t)((int32_t)" << R(instr.rs) << " % (int32_t)" << R(instr.rt) << "); }\n";
        else if (instr.mnemonic == "divu")
            out << "    if (" << R(instr.rt) << ") { regs->lo = (uint32_t)((uint32_t)" << R(instr.rs) << " / (uint32_t)" << R(instr.rt) << "); regs->hi = (uint32_t)((uint32_t)" << R(instr.rs) << " % (uint32_t)" << R(instr.rt) << "); }\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::MOVE:
        if (instr.mnemonic == "mfhi") out << "    " << R(instr.rd) << " = regs->hi;\n";
        else if (instr.mnemonic == "mflo") out << "    " << R(instr.rd) << " = regs->lo;\n";
        else if (instr.mnemonic == "mthi") out << "    regs->hi = " << R(instr.rs) << ";\n";
        else if (instr.mnemonic == "mtlo") out << "    regs->lo = " << R(instr.rs) << ";\n";
        else if (instr.mnemonic == "move") out << "    " << R(instr.rd) << " = " << R(instr.rs == 0 ? instr.rt : instr.rs) << ";\n";
        else out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::BRANCH: {
        // For targets within this function → goto; outside → tail call + return
        bool in_func = instr.branch_target != 0 &&
                       instr.branch_target >= func.start_addr &&
                       instr.branch_target <= func.end_addr;
        auto GOTO = [&](const std::string& cond) {
            if (in_func)
                out << "    if (" << cond << ") goto L_" << std::hex << instr.branch_target << ";\n";
            else
                out << "    if (" << cond << ") { func_" << std::hex << instr.branch_target << "(regs); return; }\n";
        };
        auto GOTO_UNC = [&]() {
            if (in_func)
                out << "    goto L_" << std::hex << instr.branch_target << ";\n";
            else
                out << "    func_" << std::hex << instr.branch_target << "(regs); return;\n";
        };
        if (instr.mnemonic == "beq" || instr.mnemonic == "beql")
            GOTO(R(instr.rs) + " == " + R(instr.rt));
        else if (instr.mnemonic == "bne" || instr.mnemonic == "bnel")
            GOTO(R(instr.rs) + " != " + R(instr.rt));
        else if (instr.mnemonic == "blez" || instr.mnemonic == "blezl")
            GOTO("(int32_t)" + R(instr.rs) + " <= 0");
        else if (instr.mnemonic == "bgtz" || instr.mnemonic == "bgtzl")
            GOTO("(int32_t)" + R(instr.rs) + " > 0");
        else if (instr.mnemonic == "bgez" || instr.mnemonic == "bgezl")
            GOTO("(int32_t)" + R(instr.rs) + " >= 0");
        else if (instr.mnemonic == "bltz" || instr.mnemonic == "bltzl")
            GOTO("(int32_t)" + R(instr.rs) + " < 0");
        else if (instr.mnemonic == "b")
            GOTO_UNC();
        else if (instr.mnemonic == "bc1t" || instr.mnemonic == "bc1tl")
            GOTO("regs->fcr31 & 0x00800000u");
        else if (instr.mnemonic == "bc1f" || instr.mnemonic == "bc1fl")
            GOTO("!(regs->fcr31 & 0x00800000u)");
        else if (instr.mnemonic == "bgezal" || instr.mnemonic == "bgezall") {
            out << "    regs->r[31] = 0x" << std::hex << (instr.pc + 8) << "u;\n";
            GOTO("(int32_t)" + R(instr.rs) + " >= 0");
        } else if (instr.mnemonic == "bltzal" || instr.mnemonic == "bltzall") {
            out << "    regs->r[31] = 0x" << std::hex << (instr.pc + 8) << "u;\n";
            GOTO("(int32_t)" + R(instr.rs) + " < 0");
        } else
            out << "    /* TODO branch: " << instr.mnemonic << " */\n";
        break;
    }
    case InstrCategory::JUMP:
        if (instr.mnemonic == "j") {
            // Unconditional jump — tail call if outside function bounds
            bool in_func_j = instr.branch_target != 0 &&
                             instr.branch_target >= func.start_addr &&
                             instr.branch_target <= func.end_addr;
            if (in_func_j)
                out << "    goto L_" << std::hex << instr.branch_target << ";\n";
            else
                out << "    func_" << std::hex << instr.branch_target << "(regs); return;\n";
        } else if (instr.mnemonic == "jal")
            out << "    regs->r[31] = 0x" << std::hex << (instr.pc + 8) << "u; /* ra */ func_" << std::hex << instr.branch_target << "(regs);\n";
        else if (instr.mnemonic == "jr")
            out << "    return; /* jr " << "r" << (int)instr.rs << " */\n";
        else if (instr.mnemonic == "jalr")
            out << "    regs->r[" << (int)instr.rd << "] = 0x" << std::hex << (instr.pc + 8) << "u; ps2_dispatch(regs, (uint32_t)" << R(instr.rs) << ");\n";
        else
            out << "    /* TODO jump: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::SYSCALL:
        out << "    ps2_syscall(regs, 0x" << std::hex << ((instr.raw >> 6) & 0xfffff) << "u);\n";
        break;
    case InstrCategory::FLOAT: {
        // COP1 field layout: fd=shamt[10:6], fs=rd[15:11], ft=rt[20:16]
        // mtc1/mfc1 layout: GPR=rt[20:16], FPU=rd[15:11]
        auto FD = [&]() { return "regs->f[" + std::to_string(instr.shamt) + "]"; };
        auto FS = [&]() { return "regs->f[" + std::to_string(instr.rd) + "]"; };
        auto FT = [&]() { return "regs->f[" + std::to_string(instr.rt) + "]"; };
        if (instr.mnemonic == "add.s")
            out << "    " << FD() << " = " << FS() << " + " << FT() << ";\n";
        else if (instr.mnemonic == "sub.s")
            out << "    " << FD() << " = " << FS() << " - " << FT() << ";\n";
        else if (instr.mnemonic == "mul.s")
            out << "    " << FD() << " = " << FS() << " * " << FT() << ";\n";
        else if (instr.mnemonic == "div.s")
            out << "    if (" << FT() << " != 0.0f) " << FD() << " = " << FS() << " / " << FT() << ";\n";
        else if (instr.mnemonic == "mov.s" || instr.mnemonic == "mov.d")
            out << "    " << FD() << " = " << FS() << ";\n";
        else if (instr.mnemonic == "abs.s")
            out << "    " << FD() << " = fabsf(" << FS() << ");\n";
        else if (instr.mnemonic == "neg.s")
            out << "    " << FD() << " = -" << FS() << ";\n";
        else if (instr.mnemonic == "sqrt.s")
            out << "    " << FD() << " = sqrtf(" << FS() << ");\n";
        else if (instr.mnemonic == "cvt.s.w") {
            // fd = (float)(int32_t bits of fs)
            out << "    { int32_t _iv; memcpy(&_iv, &" << FS() << ", 4); " << FD() << " = (float)_iv; }\n";
        } else if (instr.mnemonic == "cvt.w.s") {
            // fd bits = (int32_t)fs
            out << "    { int32_t _iv = (int32_t)" << FS() << "; memcpy(&" << FD() << ", &_iv, 4); }\n";
        } else if (instr.mnemonic == "cvt.s.d")
            out << "    " << FD() << " = " << FS() << "; /* cvt.s.d approx */\n";
        else if (instr.mnemonic == "mfc1")
            // rt=GPR dest, rd=FPU src
            out << "    { uint32_t _fv; memcpy(&_fv, &regs->f[" << (int)instr.rd << "], 4); " << R(instr.rt) << " = (int32_t)_fv; }\n";
        else if (instr.mnemonic == "mtc1")
            // rt=GPR src, rd=FPU dest
            out << "    { uint32_t _fv = (uint32_t)(int32_t)" << R(instr.rt) << "; memcpy(&regs->f[" << (int)instr.rd << "], &_fv, 4); }\n";
        else if (instr.mnemonic == "ctc1" || instr.mnemonic == "cfc1")
            out << "    /* " << instr.mnemonic << " control reg — ignored */\n";
        else if (instr.mnemonic == "c.lt.s")
            out << "    regs->fcr31 = (" << FS() << " < " << FT() << ") ? (regs->fcr31 | 0x00800000u) : (regs->fcr31 & ~0x00800000u);\n";
        else if (instr.mnemonic == "c.le.s")
            out << "    regs->fcr31 = (" << FS() << " <= " << FT() << ") ? (regs->fcr31 | 0x00800000u) : (regs->fcr31 & ~0x00800000u);\n";
        else if (instr.mnemonic == "c.eq.s")
            out << "    regs->fcr31 = (" << FS() << " == " << FT() << ") ? (regs->fcr31 | 0x00800000u) : (regs->fcr31 & ~0x00800000u);\n";
        else if (instr.mnemonic == "c.lt.d" || instr.mnemonic == "c.le.d" || instr.mnemonic == "c.eq.d")
            out << "    regs->fcr31 = 0; /* " << instr.mnemonic << " stub */\n";
        else
            out << "    /* FLOAT TODO: " << instr.mnemonic << " " << instr.operands << " */\n";
        break;
    }
    default:
        out << "    /* UNHANDLED: " << instr.mnemonic << " " << instr.operands << " */\n";
        break;
    }

    return out.str();
}

std::string Recompiler::emit_function(const Function& func) const {
    std::ostringstream out;
    out << "// Function: " << func.name << "  [0x" << std::hex << func.start_addr
        << " - 0x" << func.end_addr << "]\n";
    out << "void func_" << std::hex << func.start_addr << "(PS2Regs* regs) {\n";

    // Only emit labels for targets that are within this function's bounds
    std::set<uint32_t> label_addrs;
    for (uint32_t t : func.jump_targets) {
        if (t >= func.start_addr && t <= func.end_addr)
            label_addrs.insert(t);
    }

    for (const auto& instr : func.instructions) {
        // Emit label if this address is a branch target
        if (label_addrs.count(instr.pc)) {
            out << "L_" << std::hex << instr.pc << ":\n";
        }
        // $zero is always 0 — enforce it
        out << "    regs->r[0] = 0;\n";
        out << emit_instruction(instr, func);
    }
    out << "}\n\n";
    return out.str();
}

bool Recompiler::emit_c(const std::string& output_path) {
    std::ofstream f(output_path);
    if (!f.is_open()) {
        std::cerr << "[RECOMP] Cannot open output: " << output_path << "\n";
        return false;
    }

    f << emit_runtime_header();

    // Collect all cross-function call targets (tail-calls + jal) that were
    // not discovered by the BFS — will be emitted as stubs after fwd decls.
    std::set<uint32_t> stub_addrs;
    for (auto& [addr, func] : m_functions) {
        for (const auto& instr : func.instructions) {
            uint32_t tgt = instr.branch_target;
            if (tgt == 0) continue;
            bool is_cross_call =
                (instr.mnemonic == "jal") ||
                ((instr.is_branch() || instr.mnemonic == "j") &&
                 (tgt < func.start_addr || tgt > func.end_addr));
            if (is_cross_call && m_functions.find(tgt) == m_functions.end()) {
                stub_addrs.insert(tgt);
            }
        }
    }

    // Forward-declare all discovered functions
    // NOTE: patch_output.py uses this marker — keep it here.
    f << "// Forward declarations\n";
    for (auto& [addr, func] : m_functions) {
        f << "void func_" << std::hex << addr << "(PS2Regs* regs);\n";
    }
    // Also forward-declare stubs so functions can call them before definition
    for (uint32_t sa : stub_addrs) {
        f << "void func_" << std::hex << sa << "(PS2Regs* regs);\n";
    }
    f << "\n";

    // Stubs for undiscovered/out-of-range call targets (after fwd decls so
    // patch_output.py doesn't strip them — it only strips up to "// Forward declarations")
    if (!stub_addrs.empty()) {
        f << "// --- Stubs for undiscovered/out-of-range call targets ---\n";
        for (uint32_t sa : stub_addrs) {
            f << "void func_" << std::hex << sa
              << "(PS2Regs* regs) { (void)regs; /* stub: addr not in ELF */ }\n";
        }
        f << "\n";
        std::cout << "[RECOMP] Emitted " << stub_addrs.size()
                  << " stubs for undiscovered functions\n";
    }

    // Emit each function
    for (auto& [addr, func] : m_functions) {
        f << emit_function(func);
    }

    // Emit ps2_dispatch — big switch mapping every known PS2 address → C function.
    // Called by jalr (indirect call via register).
    f << "// Dynamic dispatch — called by jalr instructions\n";
    f << "void ps2_dispatch(PS2Regs* regs, uint32_t addr) {\n";
    f << "    switch (addr) {\n";
    for (auto& [addr, func] : m_functions) {
        f << "    case 0x" << std::hex << addr << "u: func_" << std::hex << addr << "(regs); return;\n";
    }
    // Also include stub targets
    for (uint32_t sa : stub_addrs) {
        f << "    case 0x" << std::hex << sa << "u: func_" << std::hex << sa << "(regs); return;\n";
    }
    f << "    default: /* unknown jalr target 0x%x — skip */ (void)regs; break;\n";
    f << "    }\n";
    f << "}\n\n";

    // Emit named game entry (called by host_main.cpp)
    f << "// Game entry point — called by host_main.cpp\n";
    f << "void ps2_game_start(void) {\n";
    f << "    PS2Regs regs = {0};\n";
    f << "    func_" << std::hex << m_elf.entry_point << "(&regs);\n";
    f << "}\n";

    std::cout << "[RECOMP] Wrote " << m_functions.size()
              << " functions to " << output_path << "\n";
    return true;
}

void Recompiler::print_functions() const {
    std::cout << "\n=== Functions (" << m_functions.size() << ") ===\n";
    size_t i = 0;
    for (auto& [addr, func] : m_functions) {
        std::cout << std::hex << std::setw(8) << std::setfill('0') << addr
                  << "  " << func.name
                  << "  (" << std::dec << func.instructions.size() << " instrs)\n";
        if (++i >= 40) { std::cout << "  ... (truncated)\n"; break; }
    }
}
