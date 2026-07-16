#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstring>

#include "iso/udf_parser.h"
#include "elf/elf_loader.h"
#include "mips/disasm.h"
#include "recomp/recompiler.h"

static void print_usage(const char* prog) {
    std::cout << "PS2 Static Recompiler\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " info    <game.iso>              -- Show ISO/ELF info\n";
    std::cout << "  " << prog << " list    <game.iso>              -- List ISO files\n";
    std::cout << "  " << prog << " disasm  <game.iso> [n_instrs]   -- Disassemble entry point\n";
    std::cout << "  " << prog << " extract <game.iso> <outdir>     -- Extract main ELF\n";
    std::cout << "  " << prog << " recomp  <game.iso> <out.c>      -- Static recompile to C\n";
    std::cout << "\n";
}

static std::pair<std::vector<uint8_t>, std::string> load_elf_from_iso(const std::string& iso_path) {
    ISOParser iso(iso_path);
    if (!iso.open()) {
        std::cerr << "Cannot open ISO: " << iso_path << "\n";
        return {};
    }

    auto exe_opt = iso.find_main_executable();
    if (!exe_opt) {
        std::cerr << "Could not find main executable in ISO\n";
        return {};
    }

    std::cout << "[ISO] Main executable: " << exe_opt->name
              << "  (" << exe_opt->size / 1024 << " KB)\n";

    auto data = iso.read_file(*exe_opt);
    iso.close();
    return {std::move(data), exe_opt->name};
}

// -----------------------------------------------------------------------
// Commands
// -----------------------------------------------------------------------

int cmd_info(const std::string& iso_path) {
    ISOParser iso(iso_path);
    if (!iso.open()) return 1;

    std::string cnf = iso.read_system_cnf();
    if (!cnf.empty()) {
        std::cout << "\n=== SYSTEM.CNF ===\n" << cnf << "\n";
    }

    auto exe_opt = iso.find_main_executable();
    if (!exe_opt) { iso.close(); return 1; }

    auto data = iso.read_file(*exe_opt);
    iso.close();

    ELFLoader loader(data);
    if (!loader.parse()) return 1;
    loader.print_info();
    return 0;
}

int cmd_list(const std::string& iso_path) {
    ISOParser iso(iso_path);
    if (!iso.open()) return 1;

    auto files = iso.list_root();
    std::cout << "\n=== ISO Root ===\n";
    for (auto& f : files) {
        std::cout << (f.is_dir ? "DIR  " : "FILE ")
                  << std::left << std::setw(30) << f.name
                  << "  LBA=" << f.lba
                  << "  size=" << f.size << "\n";
    }
    iso.close();
    return 0;
}

int cmd_disasm(const std::string& iso_path, size_t n_instrs) {
    auto [data, name] = load_elf_from_iso(iso_path);
    if (data.empty()) return 1;

    ELFLoader loader(data);
    if (!loader.parse()) return 1;

    const auto& elf = loader.elf();
    uint32_t entry = elf.entry_point;

    // Find the segment containing the entry point
    for (const auto& seg : elf.segments) {
        if (!seg.executable()) continue;
        if (entry >= seg.vaddr && entry < seg.vaddr + seg.memsz) {
            uint32_t off = entry - seg.vaddr;
            std::cout << "\n=== Disassembly @ 0x" << std::hex << entry
                      << " (" << n_instrs << " instructions) ===\n";
            MIPSDisassembler::print_block(
                seg.data.data() + off,
                seg.data.size() - off,
                entry,
                n_instrs
            );
            return 0;
        }
    }

    std::cerr << "Entry point 0x" << std::hex << entry << " not in any executable segment\n";
    return 1;
}

int cmd_extract(const std::string& iso_path, const std::string& outdir) {
    ISOParser iso(iso_path);
    if (!iso.open()) return 1;

    auto exe_opt = iso.find_main_executable();
    if (!exe_opt) { iso.close(); return 1; }

    std::string out_path = outdir + "/" + exe_opt->name + ".elf";
    if (iso.extract_file(*exe_opt, out_path)) {
        std::cout << "[EXTRACT] Saved to: " << out_path << "\n";
    } else {
        std::cerr << "[EXTRACT] Failed to write: " << out_path << "\n";
        iso.close();
        return 1;
    }
    iso.close();
    return 0;
}

int cmd_recomp(const std::string& iso_path, const std::string& out_c) {
    auto [data, name] = load_elf_from_iso(iso_path);
    if (data.empty()) return 1;

    ELFLoader loader(data);
    if (!loader.parse()) return 1;

    std::cout << "\n[RECOMP] Analyzing...\n";
    Recompiler recomp(loader.elf());
    recomp.analyze();
    recomp.print_functions();

    std::cout << "\n[RECOMP] Emitting C...\n";
    if (!recomp.emit_c(out_c)) return 1;

    return 0;
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd     = argv[1];
    std::string iso_path = argv[2];

    if (cmd == "info") {
        return cmd_info(iso_path);
    }
    else if (cmd == "list") {
        return cmd_list(iso_path);
    }
    else if (cmd == "disasm") {
        size_t n = (argc >= 4) ? (size_t)std::stoul(argv[3]) : 64;
        return cmd_disasm(iso_path, n);
    }
    else if (cmd == "extract") {
        std::string outdir = (argc >= 4) ? argv[3] : ".";
        return cmd_extract(iso_path, outdir);
    }
    else if (cmd == "recomp") {
        std::string out_c = (argc >= 4) ? argv[4] : "output.c";
        return cmd_recomp(iso_path, out_c);
    }
    else {
        std::cerr << "Unknown command: " << cmd << "\n";
        print_usage(argv[0]);
        return 1;
    }
}
