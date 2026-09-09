//===- MobileCommon.h - Bounded native mobile recovery --------------------===//
#pragma once

#include "llvm/Support/JSON.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::mobile {
namespace fs = std::filesystem;

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Limits {
  uint64_t timeout = 300;
  uint64_t max_files = 20000;
  uint64_t max_bytes = uint64_t(2) * 1024 * 1024 * 1024;
  void validate() const;
};

struct Budget {
  Limits limits;
  std::chrono::steady_clock::time_point deadline;
  uint64_t remaining;
  uint64_t output_bytes = 0;
  explicit Budget(Limits limits = {});
  void tick(uint64_t count = 1);
  void output(uint64_t count);
  void check() const;
};

struct Options {
  fs::path input;
  fs::path output;
  std::string platform = "auto";
  std::string architecture = "auto";
  std::optional<std::string> artifact;
  std::optional<std::string> jadx;
  std::string executable;
  bool metadata_only = false;
  uint64_t max_functions = 0;
  Limits limits;
};

fs::path relativeMember(std::string_view name);
fs::path pathFromUTF8(std::string_view name);
std::string pathText(const fs::path &path);
std::string readFile(const fs::path &path, uint64_t max_bytes);
void writeFile(const fs::path &path, std::string_view bytes);
void writeJSON(const fs::path &path, const llvm::json::Value &value);
llvm::json::Value parseJSON(std::string_view bytes, std::string_view what);
std::string jsonText(const llvm::json::Value &value);
void validateTree(const fs::path &root, const Limits &limits,
                  uint64_t extra_bytes = 0, uint64_t extra_files = 0);
void copyTree(const fs::path &source, const fs::path &dest,
              const Limits &limits);
std::vector<fs::path>
extractZip(const fs::path &source, const fs::path &dest, const Limits &limits,
           const std::function<bool(const fs::path &)> &Select = {});
std::optional<std::string> findProgram(std::string_view name);
void runTool(
    const std::vector<std::string> &argv, const fs::path &log, uint64_t timeout,
    const fs::path &workspace = {}, const Limits &limits = {},
    const std::map<std::string, std::optional<std::string>> &environment = {});
std::string lowerASCII(std::string_view value);
std::string portableCaseKey(std::string_view value);

llvm::json::Object recoverAndroid(const Options &options,
                                  const fs::path &staging, Budget &budget);
llvm::json::Object recoverIOS(const Options &options, const fs::path &staging,
                              Budget &budget);
llvm::json::Object recover(const Options &options);
} // namespace neverd::mobile
