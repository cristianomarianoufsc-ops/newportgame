#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

// PS2 ISOs use ISO 9660 filesystem (sector size = 2048 bytes)
// Sector 16 = Primary Volume Descriptor

static constexpr uint32_t ISO_SECTOR_SIZE = 2048;
static constexpr uint32_t ISO_PVD_SECTOR  = 16;

#pragma pack(push, 1)

struct ISO9660DirRecord {
    uint8_t  length;           // Length of this record
    uint8_t  ext_attr_length;
    uint32_t lba_le;           // Location (little-endian)
    uint32_t lba_be;           // Location (big-endian)
    uint32_t size_le;          // Data length (little-endian)
    uint32_t size_be;
    uint8_t  date[7];
    uint8_t  flags;
    uint8_t  interleave_unit;
    uint8_t  interleave_gap;
    uint16_t volume_seq_le;
    uint16_t volume_seq_be;
    uint8_t  name_len;
    char     name[1];          // Variable length
};

struct ISO9660PVD {
    uint8_t  type;             // 1 = Primary Volume Descriptor
    char     id[5];            // "CD001"
    uint8_t  version;
    uint8_t  unused0;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused1[8];
    uint32_t volume_space_le;
    uint32_t volume_space_be;
    uint8_t  unused2[32];
    uint16_t volume_set_size_le;
    uint16_t volume_set_size_be;
    uint16_t volume_seq_le;
    uint16_t volume_seq_be;
    uint16_t logical_block_size_le;
    uint16_t logical_block_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint32_t path_table_lba_le;
    uint32_t path_table_opt_le;
    uint32_t path_table_lba_be;
    uint32_t path_table_opt_be;
    uint8_t  root_dir_record[34];
    // ... rest of PVD
};

#pragma pack(pop)

struct ISOFile {
    std::string name;
    uint32_t    lba;    // Logical block address
    uint32_t    size;   // Size in bytes
    bool        is_dir;
};

class ISOParser {
public:
    explicit ISOParser(const std::string& path);
    ~ISOParser();

    bool open();
    void close();

    // List all files in the root directory
    std::vector<ISOFile> list_root();

    // List files in a specific directory path (e.g. "/SLUS_XXX.XX")
    std::vector<ISOFile> list_dir(const std::string& dir_path);

    // Extract a file by name to output_path
    bool extract_file(const ISOFile& file, const std::string& output_path);

    // Read raw bytes from a file
    std::vector<uint8_t> read_file(const ISOFile& file);

    // Find the main PS2 executable (SLUS/SCES/SCPS/SLPM entry in SYSTEM.CNF)
    std::optional<ISOFile> find_main_executable();

    // Read SYSTEM.CNF content
    std::string read_system_cnf();

private:
    std::string  m_path;
    FILE*        m_fp = nullptr;

    std::vector<uint8_t> read_sector(uint32_t lba);
    std::vector<uint8_t> read_sectors(uint32_t lba, uint32_t count);
    std::vector<ISOFile> parse_directory(uint32_t lba, uint32_t size);
};
