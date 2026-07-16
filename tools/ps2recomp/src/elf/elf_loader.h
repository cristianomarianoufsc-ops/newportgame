#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// ELF32 for MIPS (PS2 Emotion Engine)
// Machine: EM_MIPS = 8, Class: ELFCLASS32, Data: ELFDATA2LSB (little-endian)

static constexpr uint8_t  ELFMAG0       = 0x7f;
static constexpr char     ELFMAG1       = 'E';
static constexpr char     ELFMAG2       = 'L';
static constexpr char     ELFMAG3       = 'F';
static constexpr uint8_t  ELFCLASS32    = 1;
static constexpr uint8_t  ELFDATA2LSB   = 1;
static constexpr uint16_t EM_MIPS       = 8;
static constexpr uint16_t ET_EXEC       = 2;

// Program header types
static constexpr uint32_t PT_NULL       = 0;
static constexpr uint32_t PT_LOAD       = 1;
static constexpr uint32_t PT_NOTE       = 4;

// Section header types
static constexpr uint32_t SHT_NULL      = 0;
static constexpr uint32_t SHT_PROGBITS  = 1;
static constexpr uint32_t SHT_SYMTAB    = 2;
static constexpr uint32_t SHT_STRTAB    = 3;

#pragma pack(push, 1)

struct Elf32_Ehdr {
    uint8_t  e_ident[16];   // Magic + class + data + version + OS/ABI
    uint16_t e_type;        // ET_EXEC, ET_REL, etc.
    uint16_t e_machine;     // EM_MIPS = 8
    uint32_t e_version;
    uint32_t e_entry;       // Entry point virtual address
    uint32_t e_phoff;       // Program header table offset
    uint32_t e_shoff;       // Section header table offset
    uint32_t e_flags;       // MIPS-specific flags
    uint16_t e_ehsize;      // ELF header size
    uint16_t e_phentsize;
    uint16_t e_phnum;       // Number of program headers
    uint16_t e_shentsize;
    uint16_t e_shnum;       // Number of section headers
    uint16_t e_shstrndx;    // Section name string table index
};

struct Elf32_Phdr {
    uint32_t p_type;    // PT_LOAD, PT_NOTE, etc.
    uint32_t p_offset;  // Offset in file
    uint32_t p_vaddr;   // Virtual address in memory
    uint32_t p_paddr;   // Physical address
    uint32_t p_filesz;  // Size in file
    uint32_t p_memsz;   // Size in memory
    uint32_t p_flags;   // PF_X | PF_W | PF_R
    uint32_t p_align;
};

struct Elf32_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
};

struct Elf32_Sym {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
};

#pragma pack(pop)

// PF flags
static constexpr uint32_t PF_X = 0x1;
static constexpr uint32_t PF_W = 0x2;
static constexpr uint32_t PF_R = 0x4;

struct ELFSegment {
    uint32_t             vaddr;
    uint32_t             paddr;
    uint32_t             filesz;
    uint32_t             memsz;
    uint32_t             flags;
    std::vector<uint8_t> data;
    bool                 executable() const { return (flags & PF_X) != 0; }
    bool                 writable()   const { return (flags & PF_W) != 0; }
};

struct ELFSection {
    std::string          name;
    uint32_t             type;
    uint32_t             addr;
    uint32_t             size;
    std::vector<uint8_t> data;
};

struct ELFSymbol {
    std::string name;
    uint32_t    value;
    uint32_t    size;
    uint8_t     type;   // STT_FUNC, STT_OBJECT, etc.
    uint8_t     bind;   // STB_LOCAL, STB_GLOBAL
};

struct ELFFile {
    uint32_t                 entry_point;
    std::vector<ELFSegment>  segments;
    std::vector<ELFSection>  sections;
    std::vector<ELFSymbol>   symbols;

    // Memory image: maps virtual address -> data
    std::vector<uint8_t>     memory;
    uint32_t                 mem_base = 0;
    uint32_t                 mem_size = 0;

    // Read a 32-bit word from the virtual address space
    std::optional<uint32_t>  read32(uint32_t vaddr) const;
    // Map vaddr to flat memory offset
    std::optional<size_t>    vaddr_to_offset(uint32_t vaddr) const;
};

class ELFLoader {
public:
    explicit ELFLoader(const std::vector<uint8_t>& data);

    // Parse and validate the ELF
    bool parse();

    // Get the parsed ELF
    const ELFFile& elf() const { return m_elf; }

    // Print summary to stdout
    void print_info() const;

private:
    const std::vector<uint8_t>& m_data;
    ELFFile                     m_elf;

    bool parse_header();
    bool parse_segments();
    bool parse_sections();
    bool parse_symbols();
    void build_memory_image();
};
