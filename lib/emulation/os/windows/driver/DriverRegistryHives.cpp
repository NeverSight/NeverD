//===- DriverRegistryHives.cpp - Shared virtual hive admission -----------===//
///
/// \file
/// Shared virtual hive admission.
///

#include "neverd/emulation/DriverRegistryHives.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringRef.h"

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "registry_hives: " + T);
}
bool within(llvm::StringRef Key, llvm::StringRef Root) {
  return Key == Root || (Key.starts_with(Root) && Key.size() > Root.size() &&
                         Key[Root.size()] == '\\');
}
} // namespace
llvm::Error validateDriverRegistryHives(const DriverOptions &O) {
  if (!O.RegistryHives)
    return llvm::Error::success();
  if (!O.Registry || !O.Version || O.Version->Major != 10 ||
      O.Version->Minor != 0 || O.Version->Build != 19041)
    return invalid("requires explicit registry and windows_version 10.0.19041");
  return validateDriverRegistryHiveMounts(O.Registry, O.RegistryHives);
}
llvm::Error validateDriverRegistryHiveMounts(
    const std::optional<std::vector<DriverRegistryKey>> &Registry,
    const std::optional<std::vector<DriverRegistryHive>> &Hives) {
  if (!Hives)
    return llvm::Error::success();
  if (!Registry)
    return invalid("requires explicit registry");
  if (Hives->size() > 16)
    return invalid("exceeds 16 hive mounts");
  std::vector<std::string> Roots;
  for (const auto &H : *Hives) {
    if (auto E = validateDriverRegistry(
            std::vector<DriverRegistryKey>{{H.Path, {}}}))
      return invalid(llvm::toString(std::move(E)));
    const auto Root = llvm::StringRef(H.Path).lower();
    // Machine/User are namespace roots, not sufficient hive identities.
    if (Root == "\\registry\\machine" || Root == "\\registry\\user")
      return invalid("requires a hive below Machine or User");
    bool Present = false;
    for (const auto &K : *Registry)
      Present |= within(llvm::StringRef(K.Path).lower(), Root);
    if (!Present)
      return invalid("mount must name an existing registry key");
    for (const auto &Other : Roots)
      if (within(Root, Other) || within(Other, Root))
        return invalid("duplicate or overlapping hive mounts are unsupported");
    Roots.push_back(Root);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
