//===- MobileRecovery.cpp - Native recovery transaction ------------------===//
#include "MobileCommon.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <array>
#include <fstream>

namespace neverd::mobile {
namespace {
std::string detectPlatform(const fs::path &source) {
  auto suffix = lowerASCII(pathText(source.extension()));
  if (fs::is_directory(source))
    return suffix == ".app" ? "ios" : "android";
  if (suffix == ".apk" || suffix == ".dex" || suffix == ".smali")
    return "android";
  if (suffix == ".ipa")
    return "ios";
  std::ifstream stream(source, std::ios::binary);
  std::array<unsigned char, 4> magic{};
  stream.read(reinterpret_cast<char *>(magic.data()), magic.size());
  if (stream.gcount() == 4) {
    uint32_t word = uint32_t(magic[0]) | uint32_t(magic[1]) << 8 |
                    uint32_t(magic[2]) << 16 | uint32_t(magic[3]) << 24;
    if (word == 0xfeedface || word == 0xfeedfacf || word == 0xcefaedfe ||
        word == 0xcffaedfe || word == 0xcafebabe || word == 0xbebafeca ||
        word == 0xcafebabf || word == 0xbfbafeca)
      return "ios";
  }
  throw Error(
      "unsupported input; expected APK, DEX, smali, IPA, .app, or Mach-O");
}
bool inside(const fs::path &child, const fs::path &parent) {
  auto c = child.begin();
  for (auto p = parent.begin(); p != parent.end(); ++p, ++c)
    if (c == child.end() || *c != *p)
      return false;
  return true;
}
} // namespace

llvm::json::Object recover(const Options &requested) {
  Options options = requested;
  options.limits.validate();
  if (options.platform != "auto" && options.platform != "android" &&
      options.platform != "ios")
    throw Error("--platform must be auto, android or ios");
  if (options.architecture != "auto" && options.architecture != "arm64" &&
      options.architecture != "arm" && options.architecture != "x86_64" &&
      options.architecture != "i386")
    throw Error("--arch must be auto, arm64, arm, x86_64 or i386");
  if (options.output.empty())
    throw Error("mobile requires -o <new-output-directory>");
  auto status = fs::symlink_status(options.input);
  if (!fs::is_regular_file(status) && !fs::is_directory(status))
    throw Error(
        "input must be a regular file or directory, not a symbolic link");
  options.input = fs::canonical(options.input);
  if (fs::is_regular_file(status) &&
      fs::file_size(options.input) > options.limits.max_bytes)
    throw Error("input exceeds the size limit");
  options.output = fs::absolute(options.output);
  std::error_code ec;
  auto output_status = fs::symlink_status(options.output, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    throw fs::filesystem_error("cannot inspect output", options.output, ec);
  if (fs::exists(output_status) || fs::is_symlink(output_status))
    throw Error("output already exists; choose a new directory");
  options.output = fs::weakly_canonical(options.output);
  if (fs::is_directory(status) && inside(options.output, options.input))
    throw Error("output must be outside the input directory");
  if (options.platform == "auto")
    options.platform = detectPlatform(options.input);
  if (options.platform == "android" &&
      (options.metadata_only || options.artifact ||
       options.architecture != "auto" || options.max_functions ||
       options.swift_demangle))
    throw Error("--metadata-only, --artifact, --arch, --max-func and "
                "--swift-demangle apply only to iOS");
  if (options.platform == "ios" && options.jadx)
    throw Error("--jadx applies only to Android");
  fs::create_directories(options.output.parent_path());
  llvm::SmallString<256> temporary;
  auto pattern =
      pathText(options.output.parent_path() / ".neverd-mobile-%%%%%%");
  if (auto error = llvm::sys::fs::createUniqueDirectory(pattern, temporary))
    throw Error("cannot create mobile staging directory: " + error.message());
  auto staging = pathFromUTF8(temporary.str().str());
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{staging};
  Budget budget(options.limits);
  auto report = options.platform == "android"
                    ? recoverAndroid(options, staging, budget)
                    : recoverIOS(options, staging, budget);
  report["schema_version"] = 1;
  report["source"] = pathText(options.input.filename());
  report["platform"] = options.platform;
  report["status"] = "success";
  auto payload = jsonText(llvm::json::Object(report));
  validateTree(staging, options.limits, payload.size(), 1);
  writeFile(staging / "report.json", payload);
  validateTree(staging, options.limits);
#ifdef _WIN32
  fs::rename(staging, options.output);
#else
  // Reserve the name exclusively: POSIX rename otherwise replaces an empty
  // directory that a caller created while recovery was running.
  if (!fs::create_directory(options.output))
    throw Error("output already exists; choose a new directory");
  try {
    fs::rename(staging, options.output);
  } catch (...) {
    std::error_code ignored;
    fs::remove(options.output, ignored);
    throw;
  }
#endif
  return report;
}
} // namespace neverd::mobile
