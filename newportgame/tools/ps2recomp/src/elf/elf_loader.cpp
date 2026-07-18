#include "elf_loader.h"
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <algorithm>

ELFLoader::ELFLoader(const std::vector<uint8_t>& data) : m_data(data) {}

bool ELFLoader::parse() {
    if (!parse_header())   return false;
    if (!parse_segments()) return false;
    if (!parse_sections()) return false;
    parse_symbols(); // optional
    build_memory_image();
    return true;
}

bool ELFLoader::parse_header() {
    if (m_data.size() < sizeof(Elf32_Ehdr)) {
        std::cerr << "[ELF] File too small for ELF header\n";
        return false;
    }

    const Elf32_Ehdr* ehdr = reinterpret_cast<const Elf32_Ehdr*>(m_data.data());

    // Validate magic
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        std::cerr << "[ELF] Invalid ELF magic\n";
        return false;
    }
    if (ehdr->e_ident[4] != ELFCLASS32) {
        std::cerr << "[ELF] Not ELF32\n";
        return false;
    }
    if (ehdr->e_machine != EM_MIPS) {
        std::cerr << "[ELF] Not MIPS (machine=" << ehdr->e_machine << ")\n";
        return false;
    }

    m_elf.entry_point = ehdr->e_entry;
    std::cout << "[ELF] Entry point: 0x" << std::hex << ehdr->e_entry << std::dec << "\n";
    std::cout << "[ELF] Program headers: " << ehdr->e_phnum << "\n";
    std::cout << "[ELF] Section headers: " << ehdr->e_shnum << "\n";
    return true;
}

bool ELFLoader::parse_segments() {
    const Elf32_Ehdr* ehdr = reinterpret_cast<const Elf32_Ehdr*>(m_data.data());

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        size_t off = ehdr->e_phoff + i * ehdr->e_phentsize;
        if (off + sizeof(Elf32_Phdr) > m_data.size()) break;

        const Elf32_Phdr* phdr = reinterpret_cast<const Elf32_Phdr*>(m_data.data() + off);
        if (phdr->p_type != PT_LOAD) continue;

        ELFSegment seg;
        seg.vaddr  = phdr->p_vaddr;
        seg.paddr  = phdr->p_paddr;
        seg.filesz = phdr->p_filesz;
        seg.memsz  = phdr->p_memsz;
        seg.flags  = phdr->p_flags;

        if (phdr->p_filesz > 0) {
            size_t src_start = phdr->p_offset;
            size_t src_end   = src_start + phdr->p_filesz;
            if (src_end > m_data.size()) {
                std::cerr << "[ELF] Segment " << i << " extends past file\n";
                continue;
            }
            seg.data.assign(
                m_data.begin() + src_start,
                m_data.begin() + src_end
            );
        }
        // Zero-pad to memsz (BSS)
        seg.data.resize(phdr->p_memsz, 0);

        std::cout << "[ELF] Segment " << i
                  << "  vaddr=0x" << std::hex << seg.vaddr
                  << "  size=0x"  << seg.memsz
                  << std::dec
                  << (seg.executable() ? " +X" : "")
                  << (seg.writable()   ? " +W" : "")
                  << "\n";

        m_elf.segments.push_back(std::move(seg));
    }
    return !m_elf.segments.empty();
}

bool ELFLoader::parse_sections() {
    const Elf32_Ehdr* ehdr = reinterpret_cast<const Elf32_Ehdr*>(m_data.data());
    if (ehdr->e_shnum == 0) return true;

    // Get section name string table
    std::string shstrtab;
    if (ehdr->e_shstrndx < ehdr->e_shnum) {
        size_t shstr_off = ehdr->e_shoff + ehdr->e_shstrndx * ehdr->e_shentsize;
        if (shstr_off + sizeof(Elf32_Shdr) <= m_data.size()) {
            const Elf32_Shdr* shstr_hdr = reinterpret_cast<const Elf32_Shdr*>(
                m_data.data() + shstr_off);
            if (shstr_hdr->sh_offset + shstr_hdr->sh_size <= m_data.size()) {
                shstrtab.assign(
                    reinterpret_cast<const char*>(m_data.data() + shstr_hdr->sh_offset),
                    shstr_hdr->sh_size
                );
            }
        }
    }

    for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
        size_t off = ehdr->e_shoff + i * ehdr->e_shentsize;
        if (off + sizeof(Elf32_Shdr) > m_data.size()) break;

        const Elf32_Shdr* shdr = reinterpret_cast<const Elf32_Shdr*>(m_data.data() + off);
        if (shdr->sh_type == SHT_NULL) continue;

        ELFSection sec;
        sec.type = shdr->sh_type;
        sec.addr = shdr->sh_addr;
        sec.size = shdr->sh_size;

        if (!shstrtab.empty() && shdr->sh_name < shstrtab.size()) {
            sec.name = std::string(shstrtab.c_str() + shdr->sh_name);
        }

        if (shdr->sh_type == SHT_PROGBITS && shdr->sh_size > 0) {
            if (shdr->sh_offset + shdr->sh_size <= m_data.size()) {
                sec.data.assign(
                    m_data.begin() + shdr->sh_offset,
                    m_data.begin() + shdr->sh_offset + shdr->sh_size
                );
            }
        }
        m_elf.sections.push_back(std::move(sec));
    }
    return true;
}

bool ELFLoader::parse_symbols() {
    const Elf32_Ehdr* ehdr = reinterpret_cast<const Elf32_Ehdr*>(m_data.data());

    // Find SYMTAB and its associated STRTAB
    const Elf32_Shdr* symtab_hdr   = nullptr;
    const Elf32_Shdr* strtab_hdr   = nullptr;
    std::string       strtab_data;

    for (uint16_t i = 0; i < ehdr->e_shnum; ++i) {
        size_t off = ehdr->e_shoff + i * ehdr->e_shentsize;
        if (off + sizeof(Elf32_Shdr) > m_data.size()) break;
        const Elf32_Shdr* shdr = reinterpret_cast<const Elf32_Shdr*>(m_data.data() + off);
        if (shdr->sh_type == SHT_SYMTAB) {
            symtab_hdr = shdr;
            // sh_link = index of associated STRTAB
            if (shdr->sh_link < ehdr->e_shnum) {
                size_t str_off = ehdr->e_shoff + shdr->sh_link * ehdr->e_shentsize;
                if (str_off + sizeof(Elf32_Shdr) <= m_data.size()) {
                    strtab_hdr = reinterpret_cast<const Elf32_Shdr*>(
                        m_data.data() + str_off);
                }
            }
            break;
        }
    }

    if (!symtab_hdr) return true; // No symbols — OK

    if (strtab_hdr && strtab_hdr->sh_offset + strtab_hdr->sh_size <= m_data.size()) {
        strtab_data.assign(
            reinterpret_cast<const char*>(m_data.data() + strtab_hdr->sh_offset),
            strtab_hdr->sh_size
        );
    }

    size_t sym_count = symtab_hdr->sh_size / sizeof(Elf32_Sym);
    for (size_t i = 0; i < sym_count; ++i) {
        size_t off = symtab_hdr->sh_offset + i * sizeof(Elf32_Sym);
        if (off + sizeof(Elf32_Sym) > m_data.size()) break;
        const Elf32_Sym* sym = reinterpret_cast<const Elf32_Sym*>(m_data.data() + off);

        ELFSymbol s;
        s.value = sym->st_value;
        s.size  = sym->st_size;
        s.type  = sym->st_info & 0xf;
        s.bind  = sym->st_info >> 4;
        if (!strtab_data.empty() && sym->st_name < strtab_data.size())
            s.name = std::string(strtab_data.c_str() + sym->st_name);
        m_elf.symbols.push_back(std::move(s));
    }
    std::cout << "[ELF] Loaded " << m_elf.symbols.size() << " symbols\n";
    return true;
}

void ELFLoader::build_memory_image() {
    if (m_elf.segments.empty()) return;

    uint32_t lo = UINT32_MAX, hi = 0;
    for (auto& seg : m_elf.segments) {
        lo = std::min(lo, seg.vaddr);
        hi = std::max(hi, seg.vaddr + seg.memsz);
    }

    m_elf.mem_base = lo;
    m_elf.mem_size = hi - lo;
    m_elf.memory.assign(m_elf.mem_size, 0);

    for (auto& seg : m_elf.segments) {
        uint32_t dst_off = seg.vaddr - lo;
        size_t copy_sz = std::min((size_t)seg.memsz, seg.data.size());
        if (dst_off + copy_sz <= m_elf.memory.size()) {
            memcpy(m_elf.memory.data() + dst_off, seg.data.data(), copy_sz);
        }
    }

    std::cout << "[ELF] Memory image: 0x" << std::hex << lo
              << " - 0x" << hi << "  (" << std::dec
              << (m_elf.mem_size / 1024) << " KB)\n";
}

std::optional<uint32_t> ELFFile::read32(uint32_t vaddr) const {
    auto off = vaddr_to_offset(vaddr);
    if (!off || *off + 4 > memory.size()) return std::nullopt;
    uint32_t val;
    memcpy(&val, memory.data() + *off, 4);
    return val;
}

std::optional<size_t> ELFFile::vaddr_to_offset(uint32_t vaddr) const {
    if (vaddr < mem_base || vaddr >= mem_base + mem_size)
        return std::nullopt;
    return vaddr - mem_base;
}

void ELFLoader::print_info() const {
    std::cout << "\n=== ELF Summary ===\n";
    std::cout << "Entry: 0x" << std::hex << m_elf.entry_point << std::dec << "\n";
    std::cout << "Segments (" << m_elf.segments.size() << "):\n";
    for (size_t i = 0; i < m_elf.segments.size(); ++i) {
        const auto& seg = m_elf.segments[i];
        std::cout << "  [" << i << "] vaddr=0x" << std::hex << seg.vaddr
                  << "  memsz=0x" << seg.memsz << std::dec
                  << (seg.executable() ? " EXEC" : "")
                  << (seg.writable()   ? " WRITE" : "")
                  << "\n";
    }
    std::cout << "Symbols: " << m_elf.symbols.size() << "\n";
}
