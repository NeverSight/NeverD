//===- DriverObjects.cpp - Validated named driver inventory
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated named driver inventory.
///

#include "neverd/emulation/DriverObjects.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
namespace {
constexpr llvm::StringLiteral Prefix = "\\Driver\\";
}
bool validDriverObjectName(llvm::StringRef Name) {
  if (!Name.starts_with_insensitive(Prefix) || Name.size() <= Prefix.size() ||
      Name.size() > 256)
    return false;
  const auto Leaf = Name.drop_front(Prefix.size());
  return Leaf != "." && Leaf != ".." &&
         std::all_of(Leaf.begin(), Leaf.end(), [](unsigned char C) {
           return C > 32 && C < 127 && C != '\\' && C != '/' && C != ':';
         });
}
llvm::Error
validateDriverObjectNamespace(const std::optional<DriverObjectNamespace> &Names,
                              llvm::StringRef OwnServiceName) {
  if (!Names)
    return llvm::Error::success();
  auto Invalid = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "driver_namespace requires at most 64 unique absolute ASCII driver "
        "names distinct from the executing driver");
  };
  if (Names->EmptyDrivers.size() > 64)
    return Invalid();
  std::set<std::string> Seen;
  for (const auto &Name : Names->EmptyDrivers) {
    llvm::StringRef Text(Name);
    if (!validDriverObjectName(Text) ||
        Text.drop_front(Prefix.size()).equals_insensitive(OwnServiceName) ||
        !Seen.insert(Text.lower()).second)
      return Invalid();
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
