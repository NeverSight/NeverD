//===- DriverSections.cpp - Bounded named section inventories -------------===//
// NeverD Decompiler
///
/// \file
/// Bounded named section inventories.
///

#include "neverd/emulation/DriverSections.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <set>
namespace neverd::emulation {
namespace {
bool path(llvm::StringRef S) {
  if (S.size() < 2 || S.size() > 512 || S.front() != '\\' || S.back() == '\\')
    return false;
  while (!S.empty()) {
    S = S.drop_front();
    auto Part = S.take_until([](char C) { return C == '\\'; });
    if (Part.empty() || Part == "." || Part == ".." ||
        !std::all_of(Part.begin(), Part.end(), [](unsigned char C) {
          return C > 32 && C < 127 && C != '/' && C != ':';
        }))
      return false;
    S = S.drop_front(Part.size());
  }
  return true;
}
} // namespace
llvm::Error validateDriverSectionNamespace(
    const std::optional<DriverSectionNamespace> &Namespace) {
  if (!Namespace)
    return llvm::Error::success();
  auto Invalid = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "section_namespace requires unique absolute ASCII paths, at most 64 "
        "directories and sections, explicit parent directories, 1..1048576 "
        "bytes per section (4 MiB total), and protection 2 or 4");
  };
  if (Namespace->Directories.size() > 64 || Namespace->Sections.size() > 64)
    return Invalid();
  std::set<std::string> Directories, Names;
  for (const auto &D : Namespace->Directories) {
    if (!path(D))
      return Invalid();
    const auto Root = llvm::StringRef(D).drop_front().split('\\').first;
    if (Root.equals_insensitive("Device") ||
        Root.equals_insensitive("Driver") ||
        Root.equals_insensitive("Registry") ||
        Root.equals_insensitive("Callback") || Root.equals_insensitive("??") ||
        Root.equals_insensitive("DosDevices") ||
        !Directories.insert(llvm::StringRef(D).lower()).second)
      return Invalid();
  }
  uint64_t Total = 0;
  Names = Directories;
  auto ExactDirectory = [&](llvm::StringRef Name) {
    return std::find(Namespace->Directories.begin(),
                     Namespace->Directories.end(),
                     Name.str()) != Namespace->Directories.end();
  };
  for (const auto &S : Namespace->Sections) {
    const auto Name = llvm::StringRef(S.Name).lower();
    if (!path(S.Name) || Name == "\\device\\physicalmemory" ||
        !ExactDirectory(llvm::StringRef(S.Name).rsplit('\\').first) ||
        !Names.insert(Name).second || !S.Size || S.Size > 1048576 ||
        S.Bytes.size() > S.Size || (S.Protection != 2 && S.Protection != 4))
      return Invalid();
    Total += (S.Size + 4095) & ~uint64_t(4095);
  }
  if (Total > 4 * 1048576)
    return Invalid();
  // A declared subtree cannot contain an undeclared intermediate directory.
  for (const auto &D : Namespace->Directories)
    for (const auto &Root : Namespace->Directories)
      if (llvm::StringRef(D).starts_with_insensitive(Root + "\\") &&
          (!llvm::StringRef(D).starts_with(Root + "\\") ||
           !ExactDirectory(llvm::StringRef(D).rsplit('\\').first)))
        return Invalid();
  if (Namespace->Aliases.size() > 64)
    return Invalid();
  for (const auto &[Alias, Target] : Namespace->Aliases) {
    const auto A = llvm::StringRef(Alias).lower();
    if (!path(Alias) ||
        !ExactDirectory(llvm::StringRef(Alias).rsplit('\\').first) ||
        !ExactDirectory(Target) || !Names.insert(A).second)
      return Invalid();
    // A target directory may not itself live beneath a directory alias.
    for (const auto &[Other, Ignored] : Namespace->Aliases)
      if (llvm::StringRef(Target).equals_insensitive(Other) ||
          llvm::StringRef(Target).starts_with_insensitive(Other + "\\"))
        return Invalid();
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
