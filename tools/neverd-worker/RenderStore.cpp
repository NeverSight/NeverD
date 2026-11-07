//===- RenderStore.cpp - Renders kept between sessions ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A file is its magic and then records: a header, the key, each page's size
/// and the pages.  Reading a store walks the headers and keys and stops at the
/// first that does not fit, where a session may have ended; a record's
/// checksum is checked when the record is read.
//===----------------------------------------------------------------------===//

#include "RenderStore.h"

#include "ResourceLimits.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>
#include <system_error>

namespace neverd::worker {
namespace fs = std::filesystem;
namespace {
constexpr std::string_view FileMagic = "NDRS1\n";
constexpr std::string_view FileExtension = ".ndrs";
constexpr std::string_view RecordMagic = "NDRR";

struct RecordHeader {
  char magic[4];
  std::uint32_t keyBytes;
  std::uint64_t dataBytes;
  std::uint64_t checksum;
  std::uint32_t pages;
  std::uint32_t reserved;
};
static_assert(sizeof(RecordHeader) == 32);

/// Finds what a session cut short or a disk damaged; nothing depends on it
/// otherwise.
std::uint64_t checksum(std::string_view key, std::string_view data) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const auto bytes : {key, data})
    for (const char byte : bytes) {
      hash ^= static_cast<unsigned char>(byte);
      hash *= 0x100000001b3ULL;
    }
  return hash;
}
} // namespace

RenderStore::RenderStore(fs::path folder, const std::string &identity,
                         bool writable)
    : folder_(std::move(folder)), directory_(folder_ / identity),
      writable_(writable) {
  refresh();
}

void RenderStore::refresh() {
  std::error_code error;
  for (fs::directory_iterator entry(directory_, error), end;
       !error && entry != end; entry.increment(error)) {
    if (entry->path().extension() != FileExtension)
      continue;
    auto file = std::find_if(files_.begin(), files_.end(), [&](const File &at) {
      return at.path == entry->path();
    });
    if (file == files_.end())
      file = files_.insert(files_.end(), File{entry->path(), nullptr, 0});
    walkFile(*file);
  }
}

void RenderStore::walkFile(File &file) {
  namespace limits = ::neverd::limits::worker;
  // What this session writes is known already.
  if (output_.is_open() && file.path == files_[outputFile_].path)
    return;
  std::error_code error;
  const std::uint64_t size = fs::file_size(file.path, error);
  if (error || size < FileMagic.size())
    return;
  if (!file.input) {
    auto input = std::make_unique<std::ifstream>(file.path, std::ios::binary);
    std::string magic(FileMagic.size(), '\0');
    if (!input->read(magic.data(),
                     static_cast<std::streamsize>(magic.size())) ||
        magic != FileMagic)
      return;
    file.input = std::move(input);
    file.walked = FileMagic.size();
  }
  auto &input = file.input;
  input->clear();
  input->seekg(static_cast<std::streamoff>(file.walked));
  const auto index = static_cast<std::size_t>(&file - files_.data());
  auto &at = file.walked;
  // A session may be writing the last record, or have ended within it.
  while (size - at >= sizeof(RecordHeader)) {
    RecordHeader header;
    if (!input->read(reinterpret_cast<char *>(&header), sizeof(header)))
      break;
    const std::uint64_t left = size - at - sizeof(header);
    if (std::string_view(header.magic, sizeof(header.magic)) != RecordMagic ||
        header.keyBytes > limits::kRenderStoreMaxKeyBytes ||
        header.keyBytes > left || header.dataBytes > left - header.keyBytes ||
        header.pages > header.dataBytes / sizeof(std::uint64_t))
      break;
    std::string key(header.keyBytes, '\0');
    if (!input->read(key.data(), static_cast<std::streamsize>(key.size())))
      break;
    const std::uint64_t data = at + sizeof(header) + header.keyBytes;
    bytes_ += header.dataBytes;
    records_.insert_or_assign(
        std::move(key),
        Record{index, data, header.dataBytes, header.checksum, header.pages});
    at = data + header.dataBytes;
    input->seekg(static_cast<std::streamoff>(at));
  }
}

std::vector<std::string> RenderStore::read(const std::string &key) {
  const auto found = records_.find(key);
  if (found == records_.end() || !found->second.pages)
    return {};
  const Record record = found->second;
  auto &input = *files_[record.file].input;
  input.clear();
  input.seekg(static_cast<std::streamoff>(record.offset));
  std::string data(record.dataBytes, '\0');
  std::vector<std::string> pages;
  if (input.read(data.data(), static_cast<std::streamsize>(data.size())) &&
      checksum(key, data) == record.checksum) {
    std::uint64_t at = record.pages * sizeof(std::uint64_t);
    pages.reserve(record.pages);
    for (std::uint32_t page = 0; page != record.pages; ++page) {
      std::uint64_t size;
      std::memcpy(&size, data.data() + page * sizeof(size), sizeof(size));
      if (size > data.size() - at)
        break;
      pages.push_back(data.substr(at, size));
      at += size;
    }
    if (pages.size() == record.pages && at == data.size())
      return pages;
  }
  bytes_ -= record.dataBytes;
  records_.erase(found);
  return {};
}

bool RenderStore::startFile() {
  std::error_code error;
  fs::create_directories(directory_, error);
  if (error)
    return false;
  // A project reads as one set of inputs; the renders of another are not read
  // again unless the project returns to it exactly.
  for (fs::directory_iterator other(folder_, error), end;
       !error && other != end; other.increment(error))
    if (other->path() != directory_) {
      std::error_code removal;
      fs::remove_all(other->path(), removal);
    }
  std::random_device random;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto path =
      directory_ /
      (std::to_string(
           std::chrono::duration_cast<std::chrono::seconds>(now).count()) +
       "-" + std::to_string(random()) + "-" + std::to_string(random()) +
       std::string(FileExtension));
  output_.open(path, std::ios::binary | std::ios::trunc);
  output_.write(FileMagic.data(),
                static_cast<std::streamsize>(FileMagic.size()));
  output_.flush();
  if (!output_)
    return false;
  auto input = std::make_unique<std::ifstream>(path, std::ios::binary);
  if (!*input)
    return false;
  outputFile_ = files_.size();
  outputBytes_ = FileMagic.size();
  files_.push_back(File{path, std::move(input), FileMagic.size()});
  return true;
}

bool RenderStore::write(const std::string &key,
                        const std::vector<std::string> &pages) {
  namespace limits = ::neverd::limits::worker;
  if (!writable_ || key.size() > limits::kRenderStoreMaxKeyBytes ||
      pages.size() > std::numeric_limits<std::uint32_t>::max())
    return false;
  if (!output_.is_open() && !startFile()) {
    writable_ = false;
    return false;
  }
  std::size_t total = pages.size() * sizeof(std::uint64_t);
  for (const auto &page : pages)
    total += page.size();
  std::string data;
  data.reserve(total);
  for (const auto &page : pages) {
    const std::uint64_t size = page.size();
    data.append(reinterpret_cast<const char *>(&size), sizeof(size));
  }
  for (const auto &page : pages)
    data += page;
  RecordHeader header{};
  std::memcpy(header.magic, RecordMagic.data(), sizeof(header.magic));
  header.keyBytes = static_cast<std::uint32_t>(key.size());
  header.dataBytes = data.size();
  header.checksum = checksum(key, data);
  header.pages = static_cast<std::uint32_t>(pages.size());
  output_.write(reinterpret_cast<const char *>(&header), sizeof(header));
  output_.write(key.data(), static_cast<std::streamsize>(key.size()));
  output_.write(data.data(), static_cast<std::streamsize>(data.size()));
  output_.flush();
  if (!output_) {
    writable_ = false;
    return false;
  }
  const std::uint64_t offset = outputBytes_ + sizeof(header) + key.size();
  bytes_ += data.size();
  outputBytes_ = offset + data.size();
  records_.insert_or_assign(key, Record{outputFile_, offset, data.size(),
                                        header.checksum, header.pages});
  return true;
}

} // namespace neverd::worker
