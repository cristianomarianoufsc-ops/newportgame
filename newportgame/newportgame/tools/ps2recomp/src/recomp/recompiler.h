#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "../mips/disasm.h"
#include "../elf/elf_loader.h"

// PS2 Static Recompiler
// Translates MIPS R5900 instructions to C code that can be compiled for x86-64 PC.
// The generated C file links against a thin runtime that provides:
//   - PS2 memory map emulation
//   - GS (Graphics Synthesizer) stub -> OpenGL calls
//   - IOP/BIOS stub
//   - SPU2 audio stub -> OpenAL calls

struct Function {
    uint32_t                  start_addr;
    uint32_t                  end_addr;
    std::string               name;
    std::vector<DecodedInstr> instructions;
    std::vector<uint32_t>     call_targets;   // addresses this function calls
    std::vector<uint32_t>     jump_targets;   // local branch targets
};

struct RecompOptions {
    bool emit_comments    = true;  // Add original asm as comments
    bool emit_pc_tracking = true;  // Update PC variable each instruction
    bool strict_delay_slots = true;// Honor MIPS branch delay slots
    bool use_named_funcs  = true;  // Use symbol names when available
};

class Recompiler {
public:
    Recompiler(const ELFFile& elf, const RecompOptions& opts = {});

    // Discover functions starting from entry point
    // Uses recursive descent + symbol table
    void analyze();

    // Emit C translation to output path
    bool emit_c(const std::string& output_path);

    // Print discovered functions summary
    void print_functions() const;

    size_t function_count() const { return m_functions.size(); }

private:
    const ELFFile&            m_elf;
    RecompOptions             m_opts;
    std::map<uint32_t, Function> m_functions;

    // Recursive descent function discovery
    void discover_function(uint32_t addr, const std::string& name = "");

    // Identify end of function (jr $ra or unconditional branch)
    uint32_t find_function_end(uint32_t start_addr);

    // Emit C code for a single function
    std::string emit_function(const Function& func) const;

    // Emit C code for a single instruction
    std::string emit_instruction(const DecodedInstr& instr,
                                  const Function& func) const;

    // Emit the C runtime header (register file, memory accessors, etc.)
    std::string emit_runtime_header() const;

    // Helper: resolve symbol name for an address
    std::string symbol_name(uint32_t addr) const;

    // Read 32-bit instruction word from the ELF's virtual address space
    std::optional<uint32_t> fetch_word(uint32_t vaddr) const;
};
