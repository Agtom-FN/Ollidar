#include "scanengine/record/zip.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "scanengine/core/log.h"
#include "scanengine/record/lscan.h"  // lscan::crc32, directory-skeleton constants

namespace scanengine {
namespace lscan {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMod = "record";

constexpr std::uint32_t kLocalSig = 0x04034b50u;
constexpr std::uint32_t kCentralSig = 0x02014b50u;
constexpr std::uint32_t kEocdSig = 0x06054b50u;
constexpr std::size_t kIoBufBytes = 64u * 1024u;

void put_u16(std::string* out, std::uint16_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put_u32(std::string* out, std::uint32_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
}
std::uint16_t get_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t get_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

struct PendingEntry {
  std::string name;
  std::uint32_t crc32 = 0;
  std::uint32_t size = 0;
  std::uint32_t local_header_offset = 0;
};

// Rejects zip-slip: absolute paths, drive letters, or any ".." component.
bool safe_relative_name(const std::string& name) {
  if (name.empty()) return false;
  if (name.front() == '/' || name.front() == '\\') return false;
  if (name.size() >= 2 && name[1] == ':') return false;  // "C:\..."
  std::size_t start = 0;
  for (std::size_t i = 0; i <= name.size(); ++i) {
    if (i == name.size() || name[i] == '/' || name[i] == '\\') {
      if (name.compare(start, i - start, "..") == 0) return false;
      start = i + 1;
    }
  }
  return true;
}

}  // namespace

Status zip_export(const std::string& lscan_dir, const std::string& zip_path) {
  std::error_code ec;
  if (!fs::exists(lscan_dir, ec) || !fs::is_directory(lscan_dir, ec)) {
    return set_last_error(ScanError::kFileError, "zip: '%s' is not a directory",
                          lscan_dir.c_str());
  }

  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(
           lscan_dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec)) files.push_back(it->path());
  }

  std::ofstream out(zip_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return set_last_error(ScanError::kFileError, "zip: cannot create '%s'", zip_path.c_str());
  }

  std::vector<PendingEntry> entries;
  std::vector<std::uint8_t> buf(kIoBufBytes);
  std::uint32_t offset = 0;

  for (const auto& file_path : files) {
    const std::string rel = fs::relative(file_path, lscan_dir, ec).generic_string();
    if (ec || rel.empty()) {
      ec.clear();
      continue;
    }

    // Pass 1: size + CRC32, streamed. lscan::crc32's seed argument chains
    // correctly across calls (same technique as chunk_crc() in lscan.cpp),
    // so this never holds a whole file in memory.
    std::uint32_t crc = 0;
    std::uint64_t size64 = 0;
    {
      std::ifstream in(file_path, std::ios::binary);
      if (!in) {
        return set_last_error(ScanError::kFileError, "zip: cannot read '%s'",
                              file_path.string().c_str());
      }
      while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        crc = crc32(ByteSpan(buf.data(), static_cast<std::size_t>(n)), crc);
        size64 += static_cast<std::uint64_t>(n);
      }
    }
    if (size64 > 0xFFFFFFFFull) {
      return set_last_error(ScanError::kNotSupported,
                            "zip: '%s' exceeds the 4 GiB stored-entry limit",
                            file_path.string().c_str());
    }

    PendingEntry pe;
    pe.name = rel;
    pe.crc32 = crc;
    pe.size = static_cast<std::uint32_t>(size64);
    pe.local_header_offset = offset;

    std::string local;
    put_u32(&local, kLocalSig);
    put_u16(&local, 20);    // version needed to extract
    put_u16(&local, 0);     // general purpose flag
    put_u16(&local, 0);     // compression method: stored
    put_u16(&local, 0);     // mod file time
    put_u16(&local, 0x21);  // mod file date (1980-01-01; see zip.h)
    put_u32(&local, pe.crc32);
    put_u32(&local, pe.size);  // compressed size == size (stored)
    put_u32(&local, pe.size);  // uncompressed size
    put_u16(&local, static_cast<std::uint16_t>(pe.name.size()));
    put_u16(&local, 0);  // extra field length
    local += pe.name;
    out.write(local.data(), static_cast<std::streamsize>(local.size()));
    offset += static_cast<std::uint32_t>(local.size());

    // Pass 2: copy the bytes.
    {
      std::ifstream in(file_path, std::ios::binary);
      while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        out.write(reinterpret_cast<const char*>(buf.data()), n);
      }
    }
    offset += pe.size;
    if (!out) {
      return set_last_error(ScanError::kFileError, "zip: short write to '%s'", zip_path.c_str());
    }
    entries.push_back(std::move(pe));
  }

  const std::uint32_t central_start = offset;
  for (const auto& pe : entries) {
    std::string central;
    put_u32(&central, kCentralSig);
    put_u16(&central, 20);  // version made by
    put_u16(&central, 20);  // version needed
    put_u16(&central, 0);   // flags
    put_u16(&central, 0);   // method
    put_u16(&central, 0);   // mod time
    put_u16(&central, 0x21);  // mod date
    put_u32(&central, pe.crc32);
    put_u32(&central, pe.size);
    put_u32(&central, pe.size);
    put_u16(&central, static_cast<std::uint16_t>(pe.name.size()));
    put_u16(&central, 0);  // extra length
    put_u16(&central, 0);  // comment length
    put_u16(&central, 0);  // disk number start
    put_u16(&central, 0);  // internal attrs
    put_u32(&central, 0);  // external attrs
    put_u32(&central, pe.local_header_offset);
    central += pe.name;
    out.write(central.data(), static_cast<std::streamsize>(central.size()));
    offset += static_cast<std::uint32_t>(central.size());
  }
  const std::uint32_t central_size = offset - central_start;

  std::string eocd;
  put_u32(&eocd, kEocdSig);
  put_u16(&eocd, 0);  // disk number
  put_u16(&eocd, 0);  // disk with central dir
  put_u16(&eocd, static_cast<std::uint16_t>(entries.size()));
  put_u16(&eocd, static_cast<std::uint16_t>(entries.size()));
  put_u32(&eocd, central_size);
  put_u32(&eocd, central_start);
  put_u16(&eocd, 0);  // comment length
  out.write(eocd.data(), static_cast<std::streamsize>(eocd.size()));
  out.flush();
  if (!out) {
    return set_last_error(ScanError::kFileError, "zip: short write to '%s'", zip_path.c_str());
  }

  SCAN_LOG_INFO(kMod, "zip_export: '%s' -> '%s' (%zu entries)", lscan_dir.c_str(),
               zip_path.c_str(), entries.size());
  return kOkStatus;
}

Status zip_import(const std::string& zip_path, const std::string& dest_dir) {
  std::ifstream in(zip_path, std::ios::binary | std::ios::ate);
  if (!in) return set_last_error(ScanError::kFileError, "zip: cannot open '%s'", zip_path.c_str());
  const std::streamoff file_size = in.tellg();
  if (file_size < 22) {
    return set_last_error(ScanError::kCorruptData, "zip: '%s' is too small to be a zip",
                          zip_path.c_str());
  }

  // Locate the end-of-central-directory record. Our own exporter always
  // writes a zero-length comment (EOCD is exactly the last 22 bytes), but
  // scan a bounded window backward so a comment appended by another tool
  // still works.
  const std::streamoff window = std::min<std::streamoff>(file_size, 22 + 65535);
  std::vector<std::uint8_t> tail(static_cast<std::size_t>(window));
  in.seekg(file_size - window);
  in.read(reinterpret_cast<char*>(tail.data()), window);

  std::size_t eocd_pos = tail.size();
  for (std::size_t i = tail.size() - 22;; --i) {
    if (get_u32(&tail[i]) == kEocdSig) {
      eocd_pos = i;
      break;
    }
    if (i == 0) break;
  }
  if (eocd_pos == tail.size()) {
    return set_last_error(ScanError::kCorruptData,
                          "zip: '%s' has no end-of-central-directory record", zip_path.c_str());
  }

  const std::uint16_t total_entries = get_u16(&tail[eocd_pos + 10]);
  const std::uint32_t central_offset = get_u32(&tail[eocd_pos + 16]);

  std::error_code ec;
  fs::create_directories(dest_dir, ec);
  if (ec) return set_last_error(ScanError::kFileError, "zip: cannot create '%s'", dest_dir.c_str());
  for (const char* sub : {kStreamsDir, kFramesDir, kProcessedDir, kMergedDir, kExportsDir}) {
    fs::create_directories(std::string(dest_dir) + "/" + sub, ec);
  }

  in.seekg(central_offset);
  std::vector<std::uint8_t> buf(kIoBufBytes);
  for (std::uint16_t i = 0; i < total_entries; ++i) {
    std::uint8_t ch[46];
    in.read(reinterpret_cast<char*>(ch), sizeof ch);
    if (!in || get_u32(ch) != kCentralSig) {
      return set_last_error(ScanError::kCorruptData,
                            "zip: '%s' has a malformed central directory entry", zip_path.c_str());
    }
    const std::uint16_t method = get_u16(&ch[10]);
    const std::uint32_t crc = get_u32(&ch[16]);
    const std::uint32_t comp_size = get_u32(&ch[20]);
    const std::uint32_t uncomp_size = get_u32(&ch[24]);
    const std::uint16_t name_len = get_u16(&ch[28]);
    const std::uint16_t extra_len = get_u16(&ch[30]);
    const std::uint16_t comment_len = get_u16(&ch[32]);
    const std::uint32_t local_offset = get_u32(&ch[42]);

    std::string name(name_len, '\0');
    in.read(name.data(), name_len);
    in.seekg(static_cast<std::streamoff>(extra_len) + comment_len, std::ios::cur);

    if (!safe_relative_name(name)) {
      return set_last_error(ScanError::kInvalidArgument,
                            "zip: entry '%s' escapes the destination directory", name.c_str());
    }
    if (method != 0) {
      return set_last_error(
          ScanError::kNotSupported,
          "zip: entry '%s' uses compression method %u; only stored (0) is supported", name.c_str(),
          method);
    }

    const std::streampos saved = in.tellg();
    in.seekg(local_offset);
    std::uint8_t lh[30];
    in.read(reinterpret_cast<char*>(lh), sizeof lh);
    if (!in || get_u32(lh) != kLocalSig) {
      return set_last_error(ScanError::kCorruptData, "zip: entry '%s' has a malformed local header",
                            name.c_str());
    }
    const std::uint16_t lname_len = get_u16(&lh[26]);
    const std::uint16_t lextra_len = get_u16(&lh[28]);
    in.seekg(static_cast<std::streamoff>(lname_len) + lextra_len, std::ios::cur);

    const fs::path out_path = fs::path(dest_dir) / fs::path(name);
    fs::create_directories(out_path.parent_path(), ec);
    std::ofstream of(out_path, std::ios::binary | std::ios::trunc);
    if (!of) {
      return set_last_error(ScanError::kFileError, "zip: cannot create '%s'",
                            out_path.string().c_str());
    }

    std::uint32_t running_crc = 0;
    std::uint32_t remaining = comp_size;  // == uncomp_size for stored entries
    while (remaining > 0) {
      const std::size_t chunk = std::min<std::size_t>(buf.size(), remaining);
      in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(chunk));
      if (in.gcount() != static_cast<std::streamsize>(chunk)) {
        return set_last_error(ScanError::kCorruptData, "zip: entry '%s' is truncated",
                              name.c_str());
      }
      running_crc = crc32(ByteSpan(buf.data(), chunk), running_crc);
      of.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(chunk));
      remaining -= static_cast<std::uint32_t>(chunk);
    }
    of.flush();
    if (!of) {
      return set_last_error(ScanError::kFileError, "zip: short write for '%s'",
                            out_path.string().c_str());
    }
    if (running_crc != crc || comp_size != uncomp_size) {
      return set_last_error(ScanError::kCorruptData, "zip: entry '%s' failed CRC verification",
                            name.c_str());
    }

    in.seekg(saved);
  }

  SCAN_LOG_INFO(kMod, "zip_import: '%s' -> '%s' (%u entries)", zip_path.c_str(), dest_dir.c_str(),
               total_entries);
  return kOkStatus;
}

}  // namespace lscan
}  // namespace scanengine
