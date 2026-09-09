//===- MobileArchive.cpp - Bounded native ZIP extraction ------------------===//
#include "MobileCommon.h"
#include "MobileFile.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <zlib.h>

namespace neverd::mobile {
namespace {
uint64_t integer(std::string_view bytes, size_t offset, unsigned width) {
  if (offset > bytes.size() || width > bytes.size() - offset)
    throw Error("truncated ZIP directory");
  uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i)
    value |= uint64_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
  return value;
}

struct Archive {
  std::ifstream stream;
  uint64_t size;
  explicit Archive(const fs::path &path, uint64_t limit)
      : stream(path, std::ios::binary), size(fs::file_size(path)) {
    if (!fs::is_regular_file(fs::symlink_status(path)) || !stream)
      throw Error("ZIP input must be a readable regular file");
    if (size > limit ||
        size > uint64_t(std::numeric_limits<std::streamoff>::max()))
      throw Error("archive exceeds the byte limit");
  }
  std::string read(uint64_t offset, uint64_t count) {
    if (offset > size || count > size - offset ||
        count > uint64_t(std::numeric_limits<std::streamsize>::max()))
      throw Error("ZIP range exceeds its input");
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    std::string result(static_cast<size_t>(count), '\0');
    stream.read(result.data(), static_cast<std::streamsize>(count));
    if (uint64_t(stream.gcount()) != count)
      throw Error("cannot read ZIP data");
    return result;
  }
};

std::string filename(std::string_view raw, bool utf8) {
  if (utf8) {
    if (!llvm::json::isUTF8(llvm::StringRef(raw.data(), raw.size())))
      throw Error("ZIP filename contains invalid UTF-8");
    return std::string(raw);
  }
  static constexpr uint16_t mapping[] = {
#include "MobileCP437.inc"
  };
  std::string result;
  for (unsigned char byte : raw) {
    uint32_t c = byte < 128 ? byte : mapping[byte - 128];
    if (c < 128) {
      result += static_cast<char>(c);
    } else if (c < 2048) {
      result += static_cast<char>(0xc0 | (c >> 6));
      result += static_cast<char>(0x80 | (c & 63));
    } else {
      result += static_cast<char>(0xe0 | (c >> 12));
      result += static_cast<char>(0x80 | ((c >> 6) & 63));
      result += static_cast<char>(0x80 | (c & 63));
    }
  }
  return result;
}

struct Entry {
  fs::path path;
  std::string raw_name;
  uint64_t local = 0, start = 0, compressed = 0, uncompressed = 0;
  uint32_t crc = 0;
  unsigned method = 0, flags = 0;
  bool directory = false;
  bool selected = false;
};

void zip64(std::string_view extra, uint64_t &uncompressed, uint64_t &compressed,
           uint64_t &local, uint64_t &disk) {
  bool required = uncompressed == 0xffffffff || compressed == 0xffffffff ||
                  local == 0xffffffff || disk == 0xffff;
  bool found = false;
  for (size_t offset = 0; offset < extra.size();) {
    unsigned kind = integer(extra, offset, 2),
             length = integer(extra, offset + 2, 2);
    offset += 4;
    if (length > extra.size() - offset)
      throw Error("truncated ZIP extra field");
    if (kind == 1) {
      if (found)
        throw Error("duplicate ZIP64 extra field");
      found = true;
      auto data = extra.substr(offset, length);
      size_t pos = 0;
      for (auto *value : {&uncompressed, &compressed, &local}) {
        if (*value == 0xffffffff) {
          *value = integer(data, pos, 8);
          pos += 8;
        }
      }
      if (disk == 0xffff)
        disk = integer(data, pos, 4);
    }
    offset += length;
  }
  if (required && !found)
    throw Error("ZIP64 entry is missing its size metadata");
}

void extractEntry(Archive &archive, const Entry &entry,
                  const std::optional<fs::path> &dest, Budget &budget) {
  // Directory entries can contain an encoded empty deflate stream. Validate
  // that stream and its CRC too, without opening a file at the directory path.
  std::optional<OutputFile> file;
  if (dest && !entry.directory)
    file.emplace(*dest);
  uint64_t read = 0, written = 0;
  uLong crc = crc32(0, nullptr, 0);
  auto write = [&](std::string_view bytes) {
    if (bytes.size() > entry.uncompressed - written)
      throw Error("ZIP entry exceeds its declared size");
    written += bytes.size();
    crc = crc32(crc, reinterpret_cast<const Bytef *>(bytes.data()),
                static_cast<uInt>(bytes.size()));
    if (file)
      file->write(bytes);
  };
  if (entry.method == 0) {
    if (entry.compressed != entry.uncompressed)
      throw Error("stored ZIP entry size disagrees");
    while (read < entry.compressed) {
      budget.check();
      auto bytes =
          archive.read(entry.start + read,
                       std::min<uint64_t>(65536, entry.compressed - read));
      read += bytes.size();
      write(bytes);
    }
  } else {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
      throw Error("cannot initialize ZIP decompression");
    struct Cleanup {
      z_stream &stream;
      ~Cleanup() { inflateEnd(&stream); }
    } cleanup{stream};
    std::array<char, 65536> output;
    std::string input;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
      budget.check();
      if (!stream.avail_in && read < entry.compressed) {
        input =
            archive.read(entry.start + read,
                         std::min<uint64_t>(65536, entry.compressed - read));
        read += input.size();
        stream.next_in = reinterpret_cast<Bytef *>(input.data());
        stream.avail_in = static_cast<uInt>(input.size());
      }
      stream.next_out = reinterpret_cast<Bytef *>(output.data());
      stream.avail_out = output.size();
      auto before = stream.avail_in;
      status = inflate(&stream, Z_NO_FLUSH);
      if (status != Z_OK && status != Z_STREAM_END)
        throw Error("invalid or truncated ZIP deflate stream");
      size_t count = output.size() - stream.avail_out;
      write(std::string_view(output.data(), count));
      if (!count && before == stream.avail_in && status != Z_STREAM_END)
        throw Error("truncated ZIP deflate stream");
    }
    if (read != entry.compressed || stream.avail_in)
      throw Error("ZIP deflate stream has trailing compressed data");
  }
  if (written != entry.uncompressed || crc != entry.crc)
    throw Error("ZIP entry checksum or size disagrees");
  if (file)
    file->close();
}
} // namespace

std::vector<fs::path>
extractZip(const fs::path &source, const fs::path &dest, const Limits &limits,
           const std::function<bool(const fs::path &)> &Select) {
  limits.validate();
  Budget budget(limits);
  Archive archive(source, limits.max_bytes);
  uint64_t footer_start =
      archive.size - std::min<uint64_t>(archive.size, 65557);
  auto footer = archive.read(footer_start, archive.size - footer_start);
  size_t eocd = std::string::npos;
  if (footer.size() >= 22) {
    for (size_t offset = footer.size() - 22;; --offset) {
      if (integer(footer, offset, 4) == 0x06054b50 &&
          integer(footer, offset + 20, 2) == footer.size() - offset - 22) {
        eocd = offset;
        break;
      }
      if (!offset)
        break;
    }
  }
  if (eocd == std::string::npos)
    throw Error("archive has no valid ZIP end directory");
  auto end = std::string_view(footer).substr(eocd);
  uint64_t disk = integer(end, 4, 2), directory_disk = integer(end, 6, 2);
  uint64_t disk_entries = integer(end, 8, 2),
           entries_count = integer(end, 10, 2);
  uint64_t directory_size = integer(end, 12, 4),
           directory = integer(end, 16, 4);
  uint64_t directory_end_limit = footer_start + eocd;
  if (disk || directory_disk || disk_entries != entries_count)
    throw Error("multi-volume ZIP input is unsupported");
  if (entries_count == 0xffff || directory_size == 0xffffffff ||
      directory == 0xffffffff) {
    if (directory_end_limit < 20)
      throw Error("ZIP64 locator is missing");
    auto locator = archive.read(directory_end_limit - 20, 20);
    if (integer(locator, 0, 4) != 0x07064b50 || integer(locator, 4, 4) ||
        integer(locator, 16, 4) != 1)
      throw Error("invalid ZIP64 locator");
    uint64_t offset = integer(locator, 8, 8);
    auto record = archive.read(offset, 56);
    uint64_t length = integer(record, 4, 8);
    if (integer(record, 0, 4) != 0x06064b50 || length < 44 ||
        offset > directory_end_limit - 20 ||
        directory_end_limit - 20 - offset < 12 ||
        length != directory_end_limit - 20 - offset - 12 ||
        integer(record, 16, 4) || integer(record, 20, 4) ||
        integer(record, 24, 8) != integer(record, 32, 8))
      throw Error("invalid ZIP64 end directory");
    entries_count = integer(record, 32, 8);
    directory_size = integer(record, 40, 8);
    directory = integer(record, 48, 8);
    directory_end_limit = offset;
  }
  if (entries_count > limits.max_files)
    throw Error("archive exceeds the file-count limit");
  if (directory > directory_end_limit ||
      directory_size > directory_end_limit - directory)
    throw Error("ZIP central directory exceeds its input");

  std::vector<Entry> entries;
  using PathMap = std::map<std::string, std::pair<std::string, bool>>;
  PathMap ArchivePaths, OutputPaths;
  uint64_t cursor = directory, total = 0;
  auto reserve = [&](PathMap &Paths, const fs::path &path, bool directory,
                     bool explicit_entry, bool Portable) {
    auto spelling = pathText(path);
    auto [it, inserted] =
        Paths.emplace(Portable ? portableCaseKey(spelling) : spelling,
                      std::make_pair(spelling, directory));
    if (!inserted &&
        (it->second.first != spelling || it->second.second != directory ||
         (!directory && explicit_entry)))
      throw Error("archive paths have conflicting case, duplicate files or "
                  "file/directory identities");
    if (Paths.size() > limits.max_files)
      throw Error("archive exceeds the file-count limit including directories");
  };
  std::set<std::string> explicit_paths;
  for (uint64_t i = 0; i < entries_count; ++i) {
    budget.tick();
    if (cursor > directory + directory_size ||
        directory + directory_size - cursor < 46)
      throw Error("truncated ZIP central directory");
    auto header = archive.read(cursor, 46);
    if (integer(header, 0, 4) != 0x02014b50)
      throw Error("invalid ZIP central entry");
    Entry entry;
    entry.flags = integer(header, 8, 2);
    entry.method = integer(header, 10, 2);
    entry.crc = integer(header, 16, 4);
    entry.compressed = integer(header, 20, 4);
    entry.uncompressed = integer(header, 24, 4);
    auto name_length = integer(header, 28, 2),
         extra_length = integer(header, 30, 2);
    auto comment_length = integer(header, 32, 2);
    uint64_t entry_disk = integer(header, 34, 2);
    auto attributes = integer(header, 38, 4);
    entry.local = integer(header, 42, 4);
    if (name_length + extra_length + comment_length >
        directory + directory_size - cursor - 46)
      throw Error("truncated ZIP central entry fields");
    auto fields =
        archive.read(cursor + 46, name_length + extra_length + comment_length);
    entry.raw_name = fields.substr(0, name_length);
    auto name = filename(entry.raw_name, entry.flags & 0x800);
    entry.directory = !name.empty() && name.back() == '/';
    entry.path = relativeMember(name);
    entry.selected = !Select || (!entry.directory && Select(entry.path));
    zip64(std::string_view(fields).substr(name_length, extra_length),
          entry.uncompressed, entry.compressed, entry.local, entry_disk);
    if (entry_disk || (entry.flags & (1 | 0x40 | 0x2000)))
      throw Error("encrypted or multi-volume ZIP entry is unsupported");
    if (entry.method != 0 && entry.method != 8)
      throw Error("unsupported ZIP compression method");
    if (entry.directory && entry.uncompressed)
      throw Error("ZIP directory entry contains file data");
    unsigned kind = (attributes >> 16) & 0170000;
    if ((kind && kind != 0100000 && kind != 0040000) ||
        (kind == 0040000 && !entry.directory) ||
        (kind == 0100000 && entry.directory))
      throw Error(
          "archive contains a link, special file or inconsistent entry type");
    if (entry.uncompressed > limits.max_bytes - total)
      throw Error("archive exceeds the uncompressed byte limit");
    total += entry.uncompressed;
    if (!explicit_paths.insert(pathText(entry.path)).second)
      throw Error("duplicate archive path");
    for (auto parent = entry.path.parent_path(); !parent.empty();
         parent = parent.parent_path()) {
      reserve(ArchivePaths, parent, true, false, false);
      if (entry.selected)
        reserve(OutputPaths, parent, true, false, true);
    }
    // ZIP identities are case-sensitive. Only entries written to host paths
    // need the additional portable filesystem collision check. APK resources
    // can legitimately differ only by case and remain in the original APK.
    reserve(ArchivePaths, entry.path, entry.directory, true, false);
    if (entry.selected)
      reserve(OutputPaths, entry.path, entry.directory, true, true);
    entries.push_back(std::move(entry));
    cursor += 46 + name_length + extra_length + comment_length;
  }
  if (cursor != directory + directory_size)
    throw Error("ZIP directory inventory size disagrees");

  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (auto &entry : entries) {
    budget.tick();
    if (entry.local > directory || directory - entry.local < 30)
      throw Error("ZIP local entry overlaps its directory");
    auto header = archive.read(entry.local, 30);
    if (integer(header, 0, 4) != 0x04034b50 ||
        integer(header, 6, 2) != entry.flags ||
        integer(header, 8, 2) != entry.method)
      throw Error("ZIP local header disagrees with its directory");
    auto length = integer(header, 26, 2), extra = integer(header, 28, 2);
    if (length + extra > directory - entry.local - 30)
      throw Error("ZIP local entry fields overlap the directory");
    auto fields = archive.read(entry.local + 30, length + extra);
    if (fields.substr(0, length) != entry.raw_name)
      throw Error("ZIP local filename disagrees with its directory");
    if (!(entry.flags & 8)) {
      uint64_t compressed = integer(header, 18, 4),
               uncompressed = integer(header, 22, 4);
      uint64_t local = 0, disk = 0;
      zip64(std::string_view(fields).substr(length), uncompressed, compressed,
            local, disk);
      if (compressed != entry.compressed ||
          uncompressed != entry.uncompressed ||
          integer(header, 14, 4) != entry.crc)
        throw Error("ZIP local sizes/checksum disagree with its directory");
    }
    entry.start = entry.local + 30 + length + extra;
    if (entry.compressed > directory - entry.start)
      throw Error("ZIP compressed data overlaps the directory");
    ranges.emplace_back(entry.local, entry.start + entry.compressed);
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i)
    if (ranges[i].first < ranges[i - 1].second)
      throw Error("ZIP entries overlap");

  fs::create_directories(dest);
  std::vector<fs::path> written;
  for (const auto &entry : entries) {
    budget.check();
    if (!entry.selected) {
      // Unwritten members still undergo decompression, length and CRC checks;
      // selecting code does not bypass whole-archive integrity validation.
      extractEntry(archive, entry, std::nullopt, budget);
    } else if (entry.directory) {
      extractEntry(archive, entry, dest / entry.path, budget);
      fs::create_directories(dest / entry.path);
    } else {
      extractEntry(archive, entry, dest / entry.path, budget);
      written.push_back(dest / entry.path);
    }
  }
  return written;
}
} // namespace neverd::mobile
