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
    return
R"(// ps2recomp generated output — DO NOT EDIT
// Standalone:  gcc -O2 -o game output.c -lm
// With runtime: use patch_output.py then build via runtime/CMakeLists.txt

#include "ps2_runtime.h"

)";
}

std::string Recompiler::emit_instruction(const DecodedInstr& instr,
                                          const Function& /*func*/) const
{
    std::ostringstream out;

    if (m_opts.emit_comments) {
        out << "    // " << instr.to_string() << "\n";
    }
    // Helpers — all return strings so they never touch out's format flags
    auto R   = [](uint8_t r)  -> std::string { return "regs->r[" + std::to_string(r) + "]"; };
    auto F   = [](uint8_t r)  -> std::string { return "regs->f[" + std::to_string(r) + "]"; };
    auto SE  = [](int16_t v)  -> std::string { return std::to_string((int32_t)v); };
    auto SA  = [](uint8_t s)  -> std::string { return std::to_string((int)s); };  // shift amount — always decimal
    auto HEX = [](uint32_t v) -> std::string { std::ostringstream os; os << "0x" << std::hex << v << "u"; return os.str(); };

    if (m_opts.emit_pc_tracking) {
        // Use HEX() so out stays in decimal mode
        out << "    regs->pc = " << HEX(instr.pc) << ";\n";
    }

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
        // 64-bit ALU
        else if (instr.mnemonic == "daddi" || instr.mnemonic == "daddiu")
            out << "    " << R(instr.rt) << " = " << R(instr.rs) << " + (uint64_t)(int64_t)(int16_t)(" << SE(instr.imm) << ");\n";
        else if (instr.mnemonic == "daddu" || instr.mnemonic == "dadd")
            out << "    " << R(instr.rd) << " = " << R(instr.rs) << " + " << R(instr.rt) << ";\n";
        else if (instr.mnemonic == "dsubu" || instr.mnemonic == "dsub")
            out << "    " << R(instr.rd) << " = " << R(instr.rs) << " - " << R(instr.rt) << ";\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    }
    case InstrCategory::SHIFT:
        // 32-bit shifts (result sign-extended to 64)
        // 32-bit shifts — result sign-extended to 64 bits (EE-correct)
        if (instr.mnemonic == "sll")
            out << "    " << R(instr.rd) << " = (uint64_t)(int64_t)(int32_t)((uint32_t)" << R(instr.rt) << " << " << SA(instr.shamt) << ");\n";
        else if (instr.mnemonic == "srl")
            out << "    " << R(instr.rd) << " = (uint64_t)(int64_t)(int32_t)((uint32_t)" << R(instr.rt) << " >> " << SA(instr.shamt) << ");\n";
        else if (instr.mnemonic == "sra")
            out << "    " << R(instr.rd) << " = (uint64_t)(int64_t)(int32_t)((int32_t)" << R(instr.rt) << " >> " << SA(instr.shamt) << ");\n";
        else if (instr.mnemonic == "sllv")
            out << "    " << R(instr.rd) << " = (uint64_t)(int64_t)(int32_t)((uint32_t)" << R(instr.rt) << " << (" << R(instr.rs) << " & 31));\n";
        else if (instr.mnemonic == "srlv")
            out << "    " << R(instr.rd) << " = (uint64_t)(int64_t)(int32_t)((uint32_t)" << R(instr.rt) << " >> (" << R(instr.rs) << " & 31));\n";
        else if (instr.mnemonic == "srav")
            out << "    " << R(instr.rd) << " = (uint64_t)(int64_t)(int32_t)((int32_t)" << R(instr.rt) << " >> (" << R(instr.rs) << " & 31));\n";
        // 64-bit shifts (immediate)
        else if (instr.mnemonic == "dsll")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " << " << SA(instr.shamt) << ";\n";
        else if (instr.mnemonic == "dsrl")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " >> " << SA(instr.shamt) << ";\n";
        else if (instr.mnemonic == "dsra")
            out << "    " << R(instr.rd) << " = (uint64_t)((int64_t)" << R(instr.rt) << " >> " << SA(instr.shamt) << ");\n";
        else if (instr.mnemonic == "dsll32")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " << (" << SA(instr.shamt) << " + 32);\n";
        else if (instr.mnemonic == "dsrl32")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " >> (" << SA(instr.shamt) << " + 32);\n";
        else if (instr.mnemonic == "dsra32")
            out << "    " << R(instr.rd) << " = (uint64_t)((int64_t)" << R(instr.rt) << " >> (" << SA(instr.shamt) << " + 32));\n";
        // 64-bit variable shifts
        else if (instr.mnemonic == "dsllv")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " << (" << R(instr.rs) << " & 63);\n";
        else if (instr.mnemonic == "dsrlv")
            out << "    " << R(instr.rd) << " = " << R(instr.rt) << " >> (" << R(instr.rs) << " & 63);\n";
        else if (instr.mnemonic == "dsrav")
            out << "    " << R(instr.rd) << " = (uint64_t)((int64_t)" << R(instr.rt) << " >> (" << R(instr.rs) << " & 63));\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::LOAD:
        if (instr.mnemonic == "lw")
            out << "    " << R(instr.rt) << " = (uint64_t)(int64_t)(int32_t)mem_read32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lh")
            out << "    " << R(instr.rt) << " = (uint64_t)(int64_t)(int16_t)mem_read16((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lhu")
            out << "    " << R(instr.rt) << " = (uint64_t)mem_read16((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lb")
            out << "    " << R(instr.rt) << " = (uint64_t)(int64_t)(int8_t)mem_read8((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lbu")
            out << "    " << R(instr.rt) << " = (uint64_t)mem_read8((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lwu")
            out << "    " << R(instr.rt) << " = (uint64_t)mem_read32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "ld")
            out << "    " << R(instr.rt) << " = mem_read64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "));\n";
        else if (instr.mnemonic == "lq")
            out << "    " << R(instr.rt) << " = mem_read64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << ")); /* lq: lower 64 only */\n";
        else if (instr.mnemonic == "lwc1")
            out << "    { uint32_t _fv = mem_read32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << ")); memcpy(&regs->f[" << (int)instr.rt << "], &_fv, 4); }\n";
        else if (instr.mnemonic == "ldl" || instr.mnemonic == "ldr")
            out << "    " << R(instr.rt) << " = mem_read64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << ")); /* " << instr.mnemonic << " approx */\n";
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
        else if (instr.mnemonic == "sd")
            out << "    mem_write64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), " << R(instr.rt) << ");\n";
        else if (instr.mnemonic == "sq")
            out << "    mem_write64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), " << R(instr.rt) << "); /* sq: lower 64 only */\n";
        else if (instr.mnemonic == "swc1")
            out << "    { uint32_t _fv; memcpy(&_fv, &regs->f[" << (int)instr.rt << "], 4); mem_write32((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), _fv); }\n";
        else if (instr.mnemonic == "sdl" || instr.mnemonic == "sdr")
            out << "    mem_write64((uint32_t)(" << R(instr.rs) << " + " << SE(instr.imm) << "), " << R(instr.rt) << "); /* " << instr.mnemonic << " approx */\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::MULTIPLY:
        if (instr.mnemonic == "mult")
            out << "    { int64_t r = (int64_t)(int32_t)" << R(instr.rs) << " * (int64_t)(int32_t)" << R(instr.rt) << "; regs->lo = (uint64_t)(int64_t)(int32_t)r; regs->hi = (uint64_t)(int64_t)(int32_t)(r>>32); }\n";
        else if (instr.mnemonic == "multu")
            out << "    { uint64_t r = (uint64_t)(uint32_t)" << R(instr.rs) << " * (uint64_t)(uint32_t)" << R(instr.rt) << "; regs->lo = (uint32_t)r; regs->hi = (uint32_t)(r>>32); }\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::DIVIDE:
        if (instr.mnemonic == "div")
            out << "    if ((uint32_t)" << R(instr.rt) << ") { regs->lo = (uint64_t)(int64_t)(int32_t)((int32_t)" << R(instr.rs) << " / (int32_t)" << R(instr.rt) << "); regs->hi = (uint64_t)(int64_t)(int32_t)((int32_t)" << R(instr.rs) << " % (int32_t)" << R(instr.rt) << "); }\n";
        else if (instr.mnemonic == "divu")
            out << "    if ((uint32_t)" << R(instr.rt) << ") { regs->lo = (uint32_t)" << R(instr.rs) << " / (uint32_t)" << R(instr.rt) << "; regs->hi = (uint32_t)" << R(instr.rs) << " % (uint32_t)" << R(instr.rt) << "; }\n";
        else
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::MOVE:
        if (instr.mnemonic == "mfhi") out << "    " << R(instr.rd) << " = regs->hi;\n";
        else if (instr.mnemonic == "mflo") out << "    " << R(instr.rd) << " = regs->lo;\n";
        else if (instr.mnemonic == "mthi") out << "    regs->hi = " << R(instr.rs) << ";\n";
        else if (instr.mnemonic == "mtlo") out << "    regs->lo = " << R(instr.rs) << ";\n";
        else if (instr.mnemonic == "move") out << "    " << R(instr.rd) << " = " << R(instr.rs == 0 ? instr.rt : instr.rs) << ";\n";
        else if (instr.mnemonic == "movz") out << "    if (" << R(instr.rt) << " == 0) " << R(instr.rd) << " = " << R(instr.rs) << ";\n";
        else if (instr.mnemonic == "movn") out << "    if (" << R(instr.rt) << " != 0) " << R(instr.rd) << " = " << R(instr.rs) << ";\n";
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
        else if (instr.mnemonic == "jalr") {
            // rd = return address register (default $ra = 31)
            uint8_t rd = (instr.rd != 0) ? instr.rd : 31;
            out << "    " << R(rd) << " = 0x" << std::hex << (instr.pc + 8) << "u; /* jalr ra */\n";
            out << "    ps2_dispatch((uint32_t)" << R(instr.rs) << ", regs);\n";
        }
        else
            out << "    /* TODO jump: " << instr.mnemonic << " */\n";
        break;
    case InstrCategory::SYSCALL:
        out << "    ps2_syscall(regs, 0x" << std::hex << ((instr.raw >> 6) & 0xfffff) << "u);\n";
        break;
    case InstrCategory::FLOAT: {
        // COP1 (FPU) — PS2 EE has 32-bit single-precision only (no double)
        // Operand layout for arithmetic: fd=bits[10:6], fs=rd=bits[15:11], ft=rt=bits[20:16]
        // Operand layout for mfc1/mtc1/ctc1: gpr=rt=bits[20:16], fp=rd=bits[15:11]
        uint8_t fd = (instr.raw >> 6) & 0x1f;
        uint8_t fs = instr.rd;   // bits 15-11
        uint8_t ft = instr.rt;   // bits 20-16 (also GPR for mfc1/mtc1)

        if (instr.mnemonic == "mtc1") {
            // Move from GPR(ft) to FP(fs) — bit-exact 32-bit copy
            out << "    memcpy(&" << F(fs) << ", &regs->r[" << (int)ft << "], 4);\n";
        } else if (instr.mnemonic == "mfc1") {
            // Move from FP(fs) to GPR(ft) — sign-extend 32→64
            out << "    { uint32_t _v; memcpy(&_v, &" << F(fs) << ", 4); "
                << R(ft) << " = (int64_t)(int32_t)_v; }\n";
        } else if (instr.mnemonic == "ctc1") {
            // Control transfer to FP control register (fcr31)
            out << "    regs->fcr31 = (uint32_t)" << R(ft) << "; /* ctc1 */\n";
        } else if (instr.mnemonic == "cfc1") {
            // Move from FP control register to GPR
            out << "    " << R(ft) << " = (int64_t)(int32_t)regs->fcr31; /* cfc1 */\n";
        } else if (instr.mnemonic == "mov.s") {
            out << "    " << F(fd) << " = " << F(fs) << ";\n";
        } else if (instr.mnemonic == "add.s") {
            out << "    " << F(fd) << " = " << F(fs) << " + " << F(ft) << ";\n";
        } else if (instr.mnemonic == "sub.s") {
            out << "    " << F(fd) << " = " << F(fs) << " - " << F(ft) << ";\n";
        } else if (instr.mnemonic == "mul.s") {
            out << "    " << F(fd) << " = " << F(fs) << " * " << F(ft) << ";\n";
        } else if (instr.mnemonic == "div.s") {
            // PS2 hardware: div by zero gives ±MAX_FLOAT, not inf
            out << "    " << F(fd) << " = (" << F(ft) << " != 0.0f) ? "
                << F(fs) << " / " << F(ft)
                << " : ((" << F(fs) << " < 0.0f) ? -3.402823466e+38f : 3.402823466e+38f);\n";
        } else if (instr.mnemonic == "sqrt.s") {
            out << "    " << F(fd) << " = sqrtf(fabsf(" << F(fs) << "));\n";
        } else if (instr.mnemonic == "abs.s") {
            out << "    " << F(fd) << " = fabsf(" << F(fs) << ");\n";
        } else if (instr.mnemonic == "neg.s") {
            out << "    " << F(fd) << " = -" << F(fs) << ";\n";
        } else if (instr.mnemonic == "max.s") {
            // PS2-specific: max of two floats (NaN-safe on EE)
            out << "    " << F(fd) << " = (" << F(fs) << " >= " << F(ft) << ") ? " << F(fs) << " : " << F(ft) << ";\n";
        } else if (instr.mnemonic == "min.s") {
            // PS2-specific: min of two floats
            out << "    " << F(fd) << " = (" << F(fs) << " <= " << F(ft) << ") ? " << F(fs) << " : " << F(ft) << ";\n";
        } else if (instr.mnemonic == "adda.s") {
            // PS2-specific: ACC = fs + ft
            out << "    regs->f[32 /*ACC*/] = " << F(fs) << " + " << F(ft) << "; /* adda.s */\n";
        } else if (instr.mnemonic == "suba.s") {
            out << "    regs->f[32 /*ACC*/] = " << F(fs) << " - " << F(ft) << "; /* suba.s */\n";
        } else if (instr.mnemonic == "mula.s") {
            out << "    regs->f[32 /*ACC*/] = " << F(fs) << " * " << F(ft) << "; /* mula.s */\n";
        } else if (instr.mnemonic == "madd.s") {
            out << "    " << F(fd) << " = regs->f[32 /*ACC*/] + " << F(fs) << " * " << F(ft) << "; /* madd.s */\n";
        } else if (instr.mnemonic == "msub.s") {
            out << "    " << F(fd) << " = regs->f[32 /*ACC*/] - " << F(fs) << " * " << F(ft) << "; /* msub.s */\n";
        } else if (instr.mnemonic == "madda.s") {
            out << "    regs->f[32 /*ACC*/] += " << F(fs) << " * " << F(ft) << "; /* madda.s */\n";
        } else if (instr.mnemonic == "msuba.s") {
            out << "    regs->f[32 /*ACC*/] -= " << F(fs) << " * " << F(ft) << "; /* msuba.s */\n";
        } else if (instr.mnemonic == "cvt.s.s") {
            // disasm always appends ".s"; check fmt to distinguish CVT.S.S vs CVT.S.W
            uint8_t fmt_field = (instr.raw >> 21) & 0x1f;
            if (fmt_field == 0x14) { // W (word) → CVT.S.W: int32 in FP reg → float
                out << "    { int32_t _v; memcpy(&_v, &" << F(fs) << ", 4); "
                    << F(fd) << " = (float)_v; }\n";
            } else { // S → S: NOP
                out << "    " << F(fd) << " = " << F(fs) << "; /* cvt.s.s nop */\n";
            }
        } else if (instr.mnemonic == "cvt.s.w") {
            // Convert word (int32 stored in FP reg) to float
            out << "    { int32_t _v; memcpy(&_v, &" << F(fs) << ", 4); "
                << F(fd) << " = (float)_v; }\n";
        } else if (instr.mnemonic == "cvt.w.s") {
            // Convert float to word (truncate toward zero)
            out << "    { int32_t _v = (int32_t)" << F(fs) << "; "
                << "memcpy(&" << F(fd) << ", &_v, 4); }\n";
        } else if (instr.mnemonic == "round.w.s") {
            out << "    { int32_t _v = (int32_t)roundf(" << F(fs) << "); "
                << "memcpy(&" << F(fd) << ", &_v, 4); }\n";
        } else if (instr.mnemonic == "trunc.w.s") {
            out << "    { int32_t _v = (int32_t)truncf(" << F(fs) << "); "
                << "memcpy(&" << F(fd) << ", &_v, 4); }\n";
        } else if (instr.mnemonic == "ceil.w.s") {
            out << "    { int32_t _v = (int32_t)ceilf(" << F(fs) << "); "
                << "memcpy(&" << F(fd) << ", &_v, 4); }\n";
        } else if (instr.mnemonic == "floor.w.s") {
            out << "    { int32_t _v = (int32_t)floorf(" << F(fs) << "); "
                << "memcpy(&" << F(fd) << ", &_v, 4); }\n";
        // FP compare — result goes into fcr31 bit 23 (C flag)
        } else if (instr.mnemonic == "c.eq.s" || instr.mnemonic == "c.seq.s") {
            out << "    if (" << F(fs) << " == " << F(ft)
                << ") regs->fcr31 |= (1u<<23); else regs->fcr31 &= ~(1u<<23);\n";
        } else if (instr.mnemonic == "c.lt.s" || instr.mnemonic == "c.olt.s" || instr.mnemonic == "c.nge.s") {
            out << "    if (" << F(fs) << " < " << F(ft)
                << ") regs->fcr31 |= (1u<<23); else regs->fcr31 &= ~(1u<<23);\n";
        } else if (instr.mnemonic == "c.le.s" || instr.mnemonic == "c.ole.s" || instr.mnemonic == "c.ngt.s") {
            out << "    if (" << F(fs) << " <= " << F(ft)
                << ") regs->fcr31 |= (1u<<23); else regs->fcr31 &= ~(1u<<23);\n";
        } else if (instr.mnemonic == "c.ult.s") {
            out << "    if (!(" << F(fs) << " >= " << F(ft)
                << ")) regs->fcr31 |= (1u<<23); else regs->fcr31 &= ~(1u<<23);\n";
        } else if (instr.mnemonic == "c.ule.s") {
            out << "    if (!(" << F(fs) << " > " << F(ft)
                << ")) regs->fcr31 |= (1u<<23); else regs->fcr31 &= ~(1u<<23);\n";
        } else if (instr.mnemonic == "c.f.s" || instr.mnemonic == "c.sf.s") {
            // Always false
            out << "    regs->fcr31 &= ~(1u<<23); /* " << instr.mnemonic << " always false */\n";
        } else if (instr.mnemonic == "c.un.s") {
            // Unordered (NaN check)
            out << "    if (isnan(" << F(fs) << ") || isnan(" << F(ft)
                << ")) regs->fcr31 |= (1u<<23); else regs->fcr31 &= ~(1u<<23);\n";
        } else {
            out << "    /* TODO: " << instr.mnemonic << " */\n";
        }
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

    // Collect the set of PCs that actually exist in this function
    std::set<uint32_t> local_pcs;
    for (const auto& instr : func.instructions)
        local_pcs.insert(instr.pc);

    // Only emit labels for targets that are local to this function
    std::set<uint32_t> label_addrs;
    for (uint32_t t : func.jump_targets)
        if (local_pcs.count(t))
            label_addrs.insert(t);

    for (const auto& instr : func.instructions) {
        if (label_addrs.count(instr.pc))
            out << "L_" << std::hex << instr.pc << ":\n";
        // $zero is always 0 — enforce it
        out << "    regs->r[0] = 0;\n";
        out << emit_instruction(instr, func);
    }

    // For any jump_target outside this function, emit ONE tail-dispatch stub per address
    {
        std::set<uint32_t> emitted_stubs;
        bool header_done = false;
        for (uint32_t t : func.jump_targets) {
            if (!local_pcs.count(t) && !emitted_stubs.count(t)) {
                if (!header_done) {
                    out << "    /* --- cross-function branch stubs --- */\n";
                    header_done = true;
                }
                out << "L_" << std::hex << t << ": ps2_dispatch(0x" << std::hex << t << "u, regs); return;\n";
                emitted_stubs.insert(t);
            }
        }
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

    // Emit dynamic dispatch table for jalr support
    f << "// Dynamic dispatch — resolves jalr targets at runtime\n";
    f << "static void ps2_dispatch(uint32_t addr, PS2Regs* regs) {\n";
    f << "    switch (addr) {\n";
    for (auto& [addr, func] : m_functions) {
        f << "    case 0x" << std::hex << addr << "u: func_" << std::hex << addr << "(regs); return;\n";
    }
    f << "    default: break; /* unknown jalr target — no-op */\n";
    f << "    }\n";
    f << "}\n\n";

    // Emit each function
    for (auto& [addr, func] : m_functions) {
        f << emit_function(func);
    }

    // Emit named game entry (called by host_main.cpp)
    f << "// Game entry point — called by host_main.cpp\n";
    f << "void ps2_game_start(void) {\n";
    f << "    PS2Regs regs = {0};\n";
    f << "    func_" << std::hex << m_elf.entry_point << "(&regs);\n";
    f << "}\n\n";
    // Keep a standalone main for direct compilation (without runtime)
    f << "#ifndef PS2_RECOMP_HAS_HOST\n";
    f << "int main(void) { ps2_game_start(); return 0; }\n";
    f << "#endif\n";

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
