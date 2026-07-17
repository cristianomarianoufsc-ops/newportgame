#include "udf_parser.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <sstream>

ISOParser::ISOParser(const std::string& path) : m_path(path) {}

ISOParser::~ISOParser() {
    close();
}

bool ISOParser::open() {
    m_fp = fopen(m_path.c_str(), "rb");
    if (!m_fp) {
        std::cerr << "[ISO] Failed to open: " << m_path << "\n";
        return false;
    }
    return true;
}

void ISOParser::close() {
    if (m_fp) {
        fclose(m_fp);
        m_fp = nullptr;
    }
}

std::vector<uint8_t> ISOParser::read_sector(uint32_t lba) {
    return read_sectors(lba, 1);
}

std::vector<uint8_t> ISOParser::read_sectors(uint32_t lba, uint32_t count) {
    std::vector<uint8_t> buf(ISO_SECTOR_SIZE * count);
    uint64_t offset = (uint64_t)lba * ISO_SECTOR_SIZE;
    if (fseek(m_fp, (long)offset, SEEK_SET) != 0) {
        throw std::runtime_error("seek failed at LBA " + std::to_string(lba));
    }
    size_t read = fread(buf.data(), 1, buf.size(), m_fp);
    if (read != buf.size()) {
        buf.resize(read);
    }
    return buf;
}

std::vector<ISOFile> ISOParser::parse_directory(uint32_t lba, uint32_t size) {
    std::vector<ISOFile> files;
    uint32_t sectors = (size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    auto data = read_sectors(lba, sectors);

    size_t offset = 0;
    while (offset < data.size()) {
        const uint8_t* ptr = data.data() + offset;
        uint8_t rec_len = ptr[0];

        if (rec_len == 0) {
            // Advance to next sector boundary
            size_t next = (offset / ISO_SECTOR_SIZE + 1) * ISO_SECTOR_SIZE;
            if (next >= data.size()) break;
            offset = next;
            continue;
        }

        const ISO9660DirRecord* rec = reinterpret_cast<const ISO9660DirRecord*>(ptr);
        uint8_t name_len = rec->name_len;

        // Skip "." and ".." entries
        if (name_len == 1 && (rec->name[0] == '\x00' || rec->name[0] == '\x01')) {
            offset += rec_len;
            continue;
        }

        ISOFile f;
        f.lba    = rec->lba_le;
        f.size   = rec->size_le;
        f.is_dir = (rec->flags & 0x02) != 0;

        // Copy name, stripping the ";1" version suffix
        std::string raw_name(rec->name, name_len);
        auto semi = raw_name.find(';');
        if (semi != std::string::npos)
            raw_name = raw_name.substr(0, semi);
        f.name = raw_name;

        files.push_back(f);
        offset += rec_len;
    }
    return files;
}

std::vector<ISOFile> ISOParser::list_root() {
    auto pvd_data = read_sector(ISO_PVD_SECTOR);
    const ISO9660PVD* pvd = reinterpret_cast<const ISO9660PVD*>(pvd_data.data());

    if (pvd->type != 1 || memcmp(pvd->id, "CD001", 5) != 0) {
        throw std::runtime_error("Invalid ISO 9660 PVD");
    }

    const ISO9660DirRecord* root = reinterpret_cast<const ISO9660DirRecord*>(pvd->root_dir_record);
    return parse_directory(root->lba_le, root->size_le);
}

std::vector<ISOFile> ISOParser::list_dir(const std::string& dir_path) {
    // Simple one-level path resolution
    auto root_files = list_root();

    if (dir_path.empty() || dir_path == "/") {
        return root_files;
    }

    std::string name = dir_path;
    // Strip leading slash
    if (!name.empty() && name[0] == '/')
        name = name.substr(1);
    // Uppercase for ISO 9660 comparison
    for (auto& c : name) c = (char)toupper(c);

    for (auto& f : root_files) {
        if (f.is_dir && f.name == name) {
            return parse_directory(f.lba, f.size);
        }
    }
    return {};
}

std::vector<uint8_t> ISOParser::read_file(const ISOFile& file) {
    if (file.size == 0) return {};
    uint32_t sectors = (file.size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    auto data = read_sectors(file.lba, sectors);
    data.resize(file.size);
    return data;
}

bool ISOParser::extract_file(const ISOFile& file, const std::string& output_path) {
    auto data = read_file(file);
    FILE* out = fopen(output_path.c_str(), "wb");
    if (!out) return false;
    fwrite(data.data(), 1, data.size(), out);
    fclose(out);
    return true;
}

std::string ISOParser::read_system_cnf() {
    auto files = list_root();
    for (auto& f : files) {
        if (!f.is_dir && f.name == "SYSTEM.CNF") {
            auto data = read_file(f);
            return std::string(reinterpret_cast<const char*>(data.data()), data.size());
        }
    }
    return "";
}

std::optional<ISOFile> ISOParser::find_main_executable() {
    std::string cnf = read_system_cnf();
    if (cnf.empty()) {
        std::cerr << "[ISO] SYSTEM.CNF not found\n";
        return std::nullopt;
    }

    // SYSTEM.CNF format: BOOT2 = cdrom0:\SCES_516.88;1
    std::string boot_entry;
    std::istringstream ss(cnf);
    std::string line;
    while (std::getline(ss, line)) {
        // Strip carriage returns
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.find("BOOT2") != std::string::npos) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                boot_entry = line.substr(eq + 1);
                // Trim whitespace
                auto start = boot_entry.find_first_not_of(" \t");
                if (start != std::string::npos)
                    boot_entry = boot_entry.substr(start);
            }
            break;
        }
    }

    if (boot_entry.empty()) {
        std::cerr << "[ISO] BOOT2 entry not found in SYSTEM.CNF\n";
        return std::nullopt;
    }

    std::cout << "[ISO] BOOT2 = " << boot_entry << "\n";

    // Extract filename: "cdrom0:\SCES_516.88;1" -> "SCES_516.88"
    std::string filename;
    auto backslash = boot_entry.rfind('\\');
    auto slash     = boot_entry.rfind('/');
    size_t sep = std::string::npos;
    if (backslash != std::string::npos) sep = backslash;
    if (slash != std::string::npos && (sep == std::string::npos || slash > sep)) sep = slash;

    if (sep != std::string::npos)
        filename = boot_entry.substr(sep + 1);
    else
        filename = boot_entry;

    // Strip ;1 suffix
    auto semi = filename.find(';');
    if (semi != std::string::npos)
        filename = filename.substr(0, semi);

    // Uppercase
    for (auto& c : filename) c = (char)toupper(c);

    // Search in root
    auto root = list_root();
    for (auto& f : root) {
        if (!f.is_dir && f.name == filename) {
            return f;
        }
    }

    std::cerr << "[ISO] Executable not found in root: " << filename << "\n";
    return std::nullopt;
}
