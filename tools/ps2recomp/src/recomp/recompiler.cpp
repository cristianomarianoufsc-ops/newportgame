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

    // Seed from entry point
    discover_function(m_elf.entry_point, "ps2_entry");

    // Seed from symbol table (named functions)
    for (auto& sym : m_elf.symbols) {
        // STT_FUNC = 2
        if ((sym.type & 0xf) == 2 && sym.value != 0 && !sym.name.empty()) {
            if (m_functions.find(sym.value) == m_functions.end()) {
                discover_function(sym.value, sym.name);
            }
        }
    }

    std::cout << "[RECOMP] Discovered " << m_functions.size() << " functions\n";
}

void Recompiler::discover_function(uint32_t addr, const std::string& name) {
    if (m_functions.count(addr)) return;

    // Sanity check: must be in the ELF memory range
    if (!m_elf.vaddr_to_offset(addr)) return;

    Function func;
    func.start_addr = addr;
    func.name = name.empty() ? ("func_" + [&]{ std::ostringstream os; os << std::hex << addr; return os.str(); }()) : name;

    uint32_t end = find_function_end(addr);
    func.end_addr = end;

    // Disassemble the function
    uint32_t pc = addr;
    bool hit_jr_ra = false;

    while (pc <= end) {
        auto word_opt = m_elf.read32(pc);
        if (!word_opt) break;

        auto instr = MIPSDisassembler::disassemble(*word_opt, pc);
        func.instructions.push_back(instr);

        // Track call targets for further discovery
        if (instr.mnemonic == "jal" && instr.branch_target != 0) {
            func.call_targets.push_back(instr.branch_target);
        }
        if (instr.is_branch() || instr.is_jump()) {
            if (instr.branch_target != 0) {
                func.jump_targets.push_back(instr.branch_target);
            }
        }

        // JR $ra terminates function (after delay slot)
        if (instr.mnemonic == "jr" && instr.rs == 31) {
            hit_jr_ra = true;
        } else if (hit_jr_ra) {
            // This was the delay slot — done
            pc += 4;
            break;
        }
        pc += 4;
    }

    m_functions[addr] = std::move(func);

    // Recursively discover callees
    for (uint32_t target : m_functions[addr].call_targets) {
        discover_function(target);
    }
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
                                          const Function& /*func*/) const
{
    std::ostringstream out;

    if (m_opts.emit_comments) {
        out << "    // " << instr.to_string() << "\n";
    }
    if (m_opts.emit_pc_tracking) {
        out << "    regs->pc = 0x" << std::hex << instr.pc << "u;\n";
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
    case InstrCategory::BRANCH:
        // Branches become goto labels; we emit them as conditional gotos
        if (instr.mnemonic == "beq" || instr.mnemonic == "beql")
            out << "    if (" << R(instr.rs) << " == " << R(instr.rt) << ") goto L_" << std::hex << instr.branch_target << ";\n";
        else if (instr.mnemonic == "bne" || instr.mnemonic == "bnel")
            out << "    if (" << R(instr.rs) << " != " << R(instr.rt) << ") goto L_" << std::hex << instr.branch_target << ";\n";
        else if (instr.mnemonic == "blez" || instr.mnemonic == "blezl")
            out << "    if ((int32_t)" << R(instr.rs) << " <= 0) goto L_" << std::hex << instr.branch_target << ";\n";
        else if (instr.mnemonic == "bgtz" || instr.mnemonic == "bgtzl")
            out << "    if ((int32_t)" << R(instr.rs) << " > 0) goto L_" << std::hex << instr.branch_target << ";\n";
        else if (instr.mnemonic == "bgez" || instr.mnemonic == "bgezl")
            out << "    if ((int32_t)" << R(instr.rs) << " >= 0) goto L_" << std::hex << instr.branch_target << ";\n";
        else if (instr.mnemonic == "bltz" || instr.mnemonic == "bltzl")
            out << "    if ((int32_t)" << R(instr.rs) << " < 0) goto L_" << std::hex << instr.branch_target << ";\n";
        else if (instr.mnemonic == "b")
            out << "    goto L_" << std::hex << instr.branch_target << ";\n";
        else
            out << "    /* TODO branch: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::JUMP:
        if (instr.mnemonic == "j")
            out << "    goto L_" << std::hex << instr.branch_target << ";\n";
        else if (instr.mnemonic == "jal")
            out << "    regs->r[31] = 0x" << std::hex << (instr.pc + 8) << "u; /* ra */ func_" << std::hex << instr.branch_target << "(regs);\n";
        else if (instr.mnemonic == "jr")
            out << "    return; /* jr " << "r" << (int)instr.rs << " */\n";
        else if (instr.mnemonic == "jalr")
            out << "    /* indirect call via " << R(instr.rs) << " — TODO dynamic dispatch */\n";
        else
            out << "    /* TODO jump: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::SYSCALL:
        out << "    ps2_syscall(regs, 0x" << std::hex << ((instr.raw >> 6) & 0xfffff) << "u);\n";
        break;
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

    // Emit label for every instruction address (needed for goto targets)
    std::set<uint32_t> label_addrs(func.jump_targets.begin(), func.jump_targets.end());

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

    // Forward-declare all functions
    f << "// Forward declarations\n";
    for (auto& [addr, func] : m_functions) {
        f << "void func_" << std::hex << addr << "(PS2Regs* regs);\n";
    }
    f << "\n";

    // Emit each function
    for (auto& [addr, func] : m_functions) {
        f << emit_function(func);
    }

    // Emit main entry
    f << "int main(void) {\n";
    f << "    PS2Regs regs = {0};\n";
    f << "    // TODO: load ELF segments into ps2_ram here\n";
    f << "    func_" << std::hex << m_elf.entry_point << "(&regs);\n";
    f << "    return 0;\n";
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
