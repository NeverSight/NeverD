//===- MobileCommon.cpp - Native mobile filesystem and budgets ------------===//
#include "MobileCommon.h"

#include "MobileFile.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Unicode.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neverd::mobile {

void Limits::validate() const {
  if (!timeout || !max_files || !max_bytes ||
      timeout > uint64_t(std::numeric_limits<int64_t>::max() / 1000000000))
    throw Error("timeout and input limits must be positive and representable");
}

Budget::Budget(Limits configured) : limits(configured) {
  limits.validate();
  auto now = std::chrono::steady_clock::now();
  auto available = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::time_point::max() - now);
  if (limits.timeout > static_cast<uint64_t>(available.count()))
    throw Error("mobile analysis deadline is not representable");
  deadline = now + std::chrono::seconds(limits.timeout);
  remaining = std::min<uint64_t>(
      20000000, std::max<uint64_t>(100000, limits.max_bytes > 5000000
                                               ? 20000000
                                               : limits.max_bytes * 4));
}

void Budget::check() const {
  if (std::chrono::steady_clock::now() > deadline)
    throw Error("mobile analysis exceeded its time budget");
}

void Budget::tick(uint64_t count) {
  check();
  if (count > remaining)
    throw Error("mobile analysis exceeded its work budget");
  remaining -= count;
}

void Budget::output(uint64_t count) {
  check();
  if (count > limits.max_bytes - output_bytes)
    throw Error("mobile generated source exceeded its byte budget");
  output_bytes += count;
}

std::string lowerASCII(std::string_view value) {
  std::string result(value);
  for (char &c : result)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return result;
}

std::string portableCaseKey(std::string_view value) {
  if (!llvm::json::isUTF8(llvm::StringRef(value.data(), value.size())))
    throw Error("filesystem path must be valid UTF-8");
  static constexpr std::pair<uint32_t, std::string_view> expansions[] = {
#include "MobileCaseFold.inc"
  };
  std::string result;
  for (size_t i = 0; i < value.size();) {
    uint32_t c = static_cast<unsigned char>(value[i++]);
    unsigned following = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    if (following)
      c &= (1u << (6 - following)) - 1;
    for (unsigned n = 0; n < following; ++n)
      c = (c << 6) | (static_cast<unsigned char>(value[i++]) & 63);
    auto found = std::lower_bound(
        std::begin(expansions), std::end(expansions), c,
        [](const auto &entry, uint32_t code) { return entry.first < code; });
    if (found != std::end(expansions) && found->first == c) {
      result.append(found->second);
      continue;
    }
    c = static_cast<uint32_t>(llvm::sys::unicode::foldCharSimple(c));
    if (c < 0x80) {
      result += static_cast<char>(c);
    } else if (c < 0x800) {
      result += static_cast<char>(0xc0 | (c >> 6));
      result += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
      result += static_cast<char>(0xe0 | (c >> 12));
      result += static_cast<char>(0x80 | ((c >> 6) & 63));
      result += static_cast<char>(0x80 | (c & 63));
    } else {
      result += static_cast<char>(0xf0 | (c >> 18));
      result += static_cast<char>(0x80 | ((c >> 12) & 63));
      result += static_cast<char>(0x80 | ((c >> 6) & 63));
      result += static_cast<char>(0x80 | (c & 63));
    }
  }
  return result;
}

fs::path pathFromUTF8(std::string_view name) {
  if (!llvm::json::isUTF8(llvm::StringRef(name.data(), name.size())))
    throw Error("filesystem path must be valid UTF-8");
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(name.data()),
                                name.size()));
}

std::string pathText(const fs::path &path) {
  auto text = path.generic_u8string();
  std::string result(reinterpret_cast<const char *>(text.data()), text.size());
  if (!llvm::json::isUTF8(result))
    throw Error("filesystem path must be valid UTF-8");
  return result;
}

fs::path relativeMember(std::string_view name) {
  if (name.empty() ||
      name.find_first_of("\\:\0", 0, 3) != std::string_view::npos)
    throw Error("unsafe input path");
  if (name.back() == '/')
    name.remove_suffix(1);
  if (name.empty())
    throw Error("unsafe input path");
  size_t cursor = 0;
  while (cursor <= name.size()) {
    size_t end = name.find('/', cursor);
    if (end == std::string_view::npos)
      end = name.size();
    auto part = name.substr(cursor, end - cursor);
    if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
        part.back() == ' ')
      throw Error("unsafe input path");
    auto base = lowerASCII(part.substr(0, part.find('.')));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
        (base.size() == 4 &&
         (base.starts_with("com") || base.starts_with("lpt")) &&
         base[3] >= '0' && base[3] <= '9'))
      throw Error("unsafe input path contains a reserved device name");
    if (end == name.size())
      break;
    cursor = end + 1;
  }
  auto path = pathFromUTF8(name);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
    throw Error("unsafe absolute input path");
  return path;
}

std::string readFile(const fs::path &path, uint64_t max_bytes) {
  if (!fs::is_regular_file(fs::symlink_status(path)))
    throw Error("input must be a regular file, not a symbolic link or device");
  auto size = fs::file_size(path);
  if (size > max_bytes || size > std::string().max_size())
    throw Error("input exceeds the configured byte limit");
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw Error("cannot open input file");
  std::string result;
  result.reserve(static_cast<size_t>(size));
  std::array<char, 65536> buffer;
  while (stream) {
    stream.read(buffer.data(), buffer.size());
    auto count = static_cast<size_t>(stream.gcount());
    if (count > max_bytes - result.size())
      throw Error("input exceeds the configured byte limit");
    result.append(buffer.data(), count);
  }
  if (!stream.eof())
    throw Error("cannot read input file");
  return result;
}

OutputFile::OutputFile(const fs::path &path) {
  fs::create_directories(path.parent_path());
#ifdef _WIN32
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw Error("cannot exclusively create output file");
  handle = reinterpret_cast<intptr_t>(file);
#else
  handle = ::open(path.c_str(),
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (handle < 0)
    throw Error("cannot exclusively create output file");
#endif
}

OutputFile::~OutputFile() {
  if (handle != -1) {
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
  }
}

void OutputFile::write(std::string_view bytes) {
  if (handle == -1)
    throw Error("output stream is closed");
#ifdef _WIN32
  size_t cursor = 0;
  while (cursor < bytes.size()) {
    DWORD written = 0;
    DWORD count =
        static_cast<DWORD>(std::min<size_t>(65536, bytes.size() - cursor));
    if (!WriteFile(reinterpret_cast<HANDLE>(handle), bytes.data() + cursor,
                   count, &written, nullptr) ||
        !written) {
      throw Error("cannot write output file");
    }
    cursor += written;
  }
#else
  size_t cursor = 0;
  while (cursor < bytes.size()) {
    auto written = ::write(static_cast<int>(handle), bytes.data() + cursor,
                           std::min<size_t>(65536, bytes.size() - cursor));
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      throw Error("cannot write output file");
    }
    cursor += static_cast<size_t>(written);
  }
#endif
}

void OutputFile::close() {
  if (handle == -1)
    return;
  auto saved = handle;
  handle = -1;
#ifdef _WIN32
  if (!CloseHandle(reinterpret_cast<HANDLE>(saved)))
#else
  if (::close(static_cast<int>(saved)))
#endif
    throw Error("cannot close output file");
}

void writeFile(const fs::path &path, std::string_view bytes) {
  OutputFile file(path);
  file.write(bytes);
  file.close();
}

std::string jsonText(const llvm::json::Value &value) {
  return llvm::formatv("{0:2}", value).str() + "\n";
}

void writeJSON(const fs::path &path, const llvm::json::Value &value) {
  writeFile(path, jsonText(value));
}

llvm::json::Value parseJSON(std::string_view bytes, std::string_view what) {
  auto parsed = llvm::json::parse(llvm::StringRef(bytes.data(), bytes.size()));
  if (!parsed) {
    llvm::consumeError(parsed.takeError());
    throw Error(std::string(what) + " contains invalid JSON");
  }
  return std::move(*parsed);
}

void validateTree(const fs::path &root, const Limits &limits,
                  uint64_t extra_bytes, uint64_t extra_files) {
  limits.validate();
  if (!fs::is_directory(fs::symlink_status(root)))
    throw Error("output root must remain a directory");
  uint64_t files = extra_files, bytes = extra_bytes;
  if (files > limits.max_files || bytes > limits.max_bytes)
    throw Error("generated output exceeds the configured limits");
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    auto status = entry.symlink_status();
    if (!fs::is_regular_file(status) && !fs::is_directory(status))
      throw Error("generated output contains a link or special file");
    if (++files > limits.max_files)
      throw Error("generated output exceeds the file-count limit");
    if (fs::is_regular_file(status)) {
      auto size = entry.file_size();
      if (size > limits.max_bytes - bytes)
        throw Error("generated output exceeds the configured byte limit");
      bytes += size;
    }
  }
}

void copyTree(const fs::path &source, const fs::path &dest,
              const Limits &limits) {
  limits.validate();
  if (!fs::is_directory(fs::symlink_status(source)))
    throw Error("input tree must be a directory, not a symbolic link");
  struct Entry {
    fs::path source;
    fs::path relative;
    bool directory;
  };
  std::vector<Entry> entries;
  uint64_t bytes = 0;
  std::map<std::string, std::string> spelling;
  for (const auto &entry : fs::recursive_directory_iterator(source)) {
    auto status = entry.symlink_status();
    if (!fs::is_regular_file(status) && !fs::is_directory(status))
      throw Error("input contains a link or special file");
    auto relative =
        relativeMember(pathText(entry.path().lexically_relative(source)));
    auto text = pathText(relative);
    auto [it, inserted] = spelling.emplace(portableCaseKey(text), text);
    if (!inserted && it->second != text)
      throw Error("input paths have conflicting case");
    if (fs::is_regular_file(status)) {
      auto size = entry.file_size();
      if (size > limits.max_bytes - bytes)
        throw Error("input directory exceeds the byte limit");
      bytes += size;
    }
    entries.push_back({entry.path(), relative, fs::is_directory(status)});
    if (entries.size() > limits.max_files)
      throw Error("input directory exceeds the file-count limit");
  }
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return a.relative < b.relative;
  });
  fs::create_directories(dest);
  for (const auto &entry : entries) {
    if (entry.directory) {
      fs::create_directories(dest / entry.relative);
    } else {
      auto bytes = readFile(entry.source, limits.max_bytes);
      writeFile(dest / entry.relative, bytes);
    }
  }
}

std::optional<std::string> findProgram(std::string_view name) {
  auto found =
      llvm::sys::findProgramByName(llvm::StringRef(name.data(), name.size()));
  if (!found)
    return std::nullopt;
  return *found;
}
} // namespace neverd::mobile
