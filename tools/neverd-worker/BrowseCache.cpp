//===- BrowseCache.cpp - Persistent read-only query facts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded atomic cache files with content checksums and exact runtime keys.
//===----------------------------------------------------------------------===//

#include "BrowseCache.h"

#include "EngineSymbols.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace neverd::worker {
namespace fs = std::filesystem;
namespace {
using Hash = decltype(&neverd_sha256_hex);
constexpr std::string_view Magic = "NDBC1\n";
constexpr std::size_t HeaderBytes = Magic.size() + 65 + 65;

fs::path modulePath(const void *address) {
#ifdef _WIN32
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module))
    throw std::runtime_error("Cannot locate cache runtime module");
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  if (!size || size >= path.size())
    throw std::runtime_error("Cannot locate cache runtime path");
  path.resize(size);
  return path;
#else
  Dl_info module{};
  if (!dladdr(address, &module) || !module.dli_fname)
    throw std::runtime_error("Cannot locate cache runtime module");
  return fs::absolute(module.dli_fname);
#endif
}

std::string
readFile(const fs::path &path,
         std::size_t limit = std::numeric_limits<std::size_t>::max()) {
  const auto size = fs::file_size(path);
  if (size > limit)
    throw std::runtime_error("Cache input exceeds its read budget");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot read cache input");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Cache input changed while reading");
  return bytes;
}

const std::string &runtimeIdentity() {
  static const auto identity = [] {
    const auto engine =
        modulePath(reinterpret_cast<const void *>(&neverd_session_create));
    const auto worker =
        modulePath(reinterpret_cast<const void *>(&runtimeIdentity));
    const auto moduleKey = [](const fs::path &path) {
#ifdef _WIN32
      // Loaded PE files cannot be rewritten in place. Bind the local cache to
      // their NTFS file identity and change time, including changes that keep
      // length and last-write time. Do not reread a 150 MiB DLL at every open.
      HANDLE file =
          CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        FILE_ID_INFO id{};
        FILE_BASIC_INFO basic{};
        LARGE_INTEGER size{};
        const bool valid =
            GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) &&
            GetFileInformationByHandleEx(file, FileBasicInfo, &basic,
                                         sizeof(basic)) &&
            GetFileSizeEx(file, &size);
        CloseHandle(file);
        if (valid) {
          std::string key(reinterpret_cast<const char *>(id.FileId.Identifier),
                          sizeof(id.FileId.Identifier));
          key += std::to_string(id.VolumeSerialNumber) + ":" +
                 std::to_string(basic.ChangeTime.QuadPart) + ":" +
                 std::to_string(basic.LastWriteTime.QuadPart) + ":" +
                 std::to_string(size.QuadPart);
          return BrowseCache::digest(key);
        }
      }
#else
      // Hashing the engine library costs about half a second at the first
      // browse request of every worker, the first paint this cache serves.
      // As on Windows, bind the cache to the file's identity: a replacement
      // has another inode, and rewriting it in place changes its ctime.
      struct stat info{};
      if (::stat(path.c_str(), &info) == 0) {
#ifdef __APPLE__
        const auto &changed = info.st_ctimespec;
        const auto &written = info.st_mtimespec;
#else
        const auto &changed = info.st_ctim;
        const auto &written = info.st_mtim;
#endif
        std::string key;
        for (const auto value :
             {std::int64_t(info.st_dev), std::int64_t(info.st_ino),
              std::int64_t(info.st_size), std::int64_t(changed.tv_sec),
              std::int64_t(changed.tv_nsec), std::int64_t(written.tv_sec),
              std::int64_t(written.tv_nsec)})
          key += std::to_string(value) + ":";
        return BrowseCache::digest(key);
      }
#endif
      return BrowseCache::digest(readFile(path));
    };
    return moduleKey(engine) + moduleKey(worker);
  }();
  return identity;
}
} // namespace

std::string BrowseCache::digest(std::string_view bytes) {
  const auto hash = engineSymbol<Hash>("neverd_sha256_hex");
  if (!hash)
    return {};
  const char *raw =
      hash(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
  if (!raw)
    throw std::runtime_error("Cache digest failed");
  std::string result(raw);
  neverd_free_string(raw);
  return result;
}

void BrowseCache::configure(fs::path projectFolder,
                            std::function<Json()> identity, bool writable,
                            AtomicWriter writer) {
  folder_ = std::move(projectFolder) / "local" / "browse-cache";
  identity_ = std::move(identity);
  writable_ = writable;
  writer_ = writer;
  invalidate();
}

void BrowseCache::invalidate() {
  pending_.clear();
  initialized_ = enabled_ = false;
  namespace_.clear();
}

bool BrowseCache::initialize() {
  if (initialized_)
    return enabled_;
  initialized_ = true;
  if (!identity_ || !engineSymbol<Hash>("neverd_sha256_hex"))
    return false;
  try {
    namespace_ = digest(runtimeIdentity() + identity_().dump());
    enabled_ = !namespace_.empty();
    diagnostic_.clear();
  } catch (const std::exception &error) {
    diagnostic_ = error.what();
  }
  return enabled_;
}

std::optional<std::string> BrowseCache::read(const std::string &key) {
  if (!initialize())
    return {};
  const auto name = digest(namespace_ + key);
  const auto path = folder_ / (name + ".ndbc");
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error) {
    ++misses_;
    return {};
  }
  try {
    if (size < HeaderBytes ||
        size - HeaderBytes >
            ::neverd::limits::worker::kBrowseCacheMaxEntryBytes)
      throw std::runtime_error("Invalid browse cache size");
    auto bytes =
        readFile(path, HeaderBytes +
                           ::neverd::limits::worker::kBrowseCacheMaxEntryBytes);
    if (bytes.size() != size || !bytes.starts_with(Magic) ||
        bytes.substr(Magic.size(), 65) != name + "\n" ||
        bytes[HeaderBytes - 1] != '\n' ||
        bytes.substr(Magic.size() + 65, 64) !=
            digest(std::string_view(bytes).substr(HeaderBytes)))
      throw std::runtime_error("Browse cache checksum mismatch");
    bytes.erase(0, HeaderBytes);
    ++hits_;
    return bytes;
  } catch (const std::exception &failure) {
    ++invalid_;
    ++misses_;
    diagnostic_ = failure.what();
    return {};
  }
}

void BrowseCache::defer(const std::string &key, Producer producer) {
  if (!initialize() || !writable_ || !writer_)
    return;
  if (pending_.size() >=
          ::neverd::limits::worker::kBrowseCacheMaxPendingEntries &&
      !pending_.contains(key))
    return;
  pending_.insert_or_assign(key, std::move(producer));
}

void BrowseCache::reject(std::string diagnostic) {
  ++invalid_;
  diagnostic_ = std::move(diagnostic);
}

Json BrowseCache::query(const std::string &key, std::function<Json()> producer,
                        std::function<bool(const Json &)> valid) {
  if (const auto cached = read(key)) {
    try {
      auto value = Json::from_cbor(*cached, true, false);
      if (!value.is_discarded() && valid(value))
        return value;
      reject("Invalid browse cache query document");
    } catch (const Json::exception &error) {
      reject(error.what());
    }
  }
  auto result = producer();
  if (writable() && !result.is_null() && valid(result))
    defer(key, [result] {
      const auto bytes = Json::to_cbor(result);
      return std::string(bytes.begin(), bytes.end());
    });
  return result;
}

void BrowseCache::flushOne() noexcept {
  if (pending_.empty())
    return;
  auto item = pending_.extract(pending_.begin());
  try {
    auto bytes = item.mapped()();
    if (bytes.empty() ||
        bytes.size() > ::neverd::limits::worker::kBrowseCacheMaxEntryBytes)
      return;
    const auto name = digest(namespace_ + item.key());
    writer_(folder_ / (name + ".ndbc"),
            std::string(Magic) + name + "\n" + digest(bytes) + "\n" + bytes);
    ++writes_;
    if (pending_.empty())
      prune();
  } catch (const std::exception &error) {
    diagnostic_ = error.what();
  }
}

void BrowseCache::flush() noexcept {
  while (!pending_.empty())
    flushOne();
}

void BrowseCache::prune() {
  std::vector<fs::directory_entry> files;
  std::uintmax_t total = 0;
  const auto staleBefore =
      fs::file_time_type::clock::now() -
      std::chrono::seconds(
          ::neverd::limits::worker::kBrowseCacheStaleTemporarySeconds);
  for (const auto &file : fs::directory_iterator(folder_)) {
    if (!file.is_regular_file())
      continue;
    // A writer stopped between writing and publishing an entry. Its temporary
    // file is neither readable nor counted, so nothing else would remove it.
    if (file.path().filename().string().find(".ndbc.tmp-") !=
        std::string::npos) {
      std::error_code error;
      const auto written = file.last_write_time(error);
      if (!error && written < staleBefore)
        fs::remove(file.path(), error);
      continue;
    }
    if (file.path().extension() == ".ndbc") {
      files.push_back(file);
      total += file.file_size();
    }
  }
  if (total <= ::neverd::limits::worker::kBrowseCacheMaxBytes)
    return;
  std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
    return a.last_write_time() < b.last_write_time();
  });
  for (const auto &file : files) {
    if (total <= ::neverd::limits::worker::kBrowseCacheMaxBytes)
      break;
    const auto size = file.file_size();
    if (fs::remove(file.path()))
      total -= size;
  }
}

Json BrowseCache::stats() const {
  return {{"enabled", enabled_},      {"hits", hits_},
          {"misses", misses_},        {"writes", writes_},
          {"invalid", invalid_},      {"pending", pending_.size()},
          {"diagnostic", diagnostic_}};
}
} // namespace neverd::worker
