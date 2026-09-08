//===- MobileIOSInternal.h - Native mobile source recovery helpers --------===//
#pragma once
#include "MobileCommon.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace neverd {
struct BinaryImage;
}
namespace neverd::mobile::ios {
using Object = llvm::json::Object;
using Array = llvm::json::Array;
using Value = llvm::json::Value;
inline uint64_t toolTimeout(Budget &budget) {
  budget.check();
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                       budget.deadline - std::chrono::steady_clock::now())
                       .count();
  return std::min(budget.limits.timeout,
                  uint64_t(std::max<int64_t>(1, (remaining + 999) / 1000)));
}
inline std::string str(const Object &o, llvm::StringRef key,
                       std::string fallback = {}) {
  auto v = o.getString(key);
  return v ? v->str() : fallback;
}
inline int64_t number(const Object &o, llvm::StringRef key,
                      int64_t fallback = 0) {
  return o.getInteger(key).value_or(fallback);
}
inline bool flag(const Object &o, llvm::StringRef key) {
  return o.getBoolean(key).value_or(false);
}
const Object &object(const Value &v, std::string_view what);
const Array &array(const Object &o, llvm::StringRef key);
std::string requiredString(const Object &o, llvm::StringRef key);
bool identifier(std::string_view s);
bool hexAddress(std::string_view s);
std::string hex(uint64_t v);
void appendUnique(Array &to, const Array &from);

struct Selection {
  std::string bytes;
  std::string architecture;
  uint32_t cpu_type = 0, cpu_subtype = 0;
  unsigned pointer_size = 0;
  bool encrypted = false;
  std::vector<std::string> available;
};
Selection selectSlice(std::string_view bytes, std::string_view architecture,
                      Budget &budget);
Object parsePlist(std::string_view bytes, Budget &budget);
Object objcMetadata(const BinaryImage &image);
Object swiftMetadata(const BinaryImage &image, Budget &budget);
std::vector<std::string> objcTypes(std::string_view encoding);
std::string objcHeader(const Object &metadata, unsigned pointer_size = 8);
struct SourceResult {
  std::string source;
  Object coverage;
};
SourceResult objcSources(const Object &batch, const Object &metadata,
                         unsigned pointer_size, Budget &budget);
uint64_t nativeFunctionCount(std::string_view source);
Object swiftSignature(std::string_view entry, std::string_view mangled,
                      std::string_view expanded, unsigned pointer_size = 8);
Object swiftSignatures(const Array &symbols, const std::string &demangler,
                       const fs::path &logs, unsigned pointer_size,
                       Budget &budget);
Object swiftCoverage(const Object &inventory, const Object *batch,
                     std::string_view workflow_status = {});
struct SwiftResult {
  Object coverage;
  Object outputs;
};
SwiftResult swiftSources(const Options &options, const fs::path &staging,
                         const fs::path &binary, const Array &symbols,
                         unsigned pointer_size, Budget &budget);
} // namespace neverd::mobile::ios
