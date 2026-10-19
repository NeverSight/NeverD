///
/// \file
/// DriverRegistryHivesJSON.
///

#include "DriverRegistryHivesJSON.h"
namespace neverd::emulation {
llvm::Expected<std::vector<DriverRegistryHive>>
parseDriverRegistryHives(const llvm::json::Value &V) {
  const auto *A = V.getAsArray();
  if (!A || A->size() > 16)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "registry_hives requires at most 16 explicit mounts");
  std::vector<DriverRegistryHive> R;
  for (const auto &Entry : *A) {
    const auto *O = Entry.getAsObject();
    auto Path = O ? O->getString("path") : std::nullopt;
    auto Storage = O ? O->getString("storage") : std::nullopt;
    if (!O || O->size() != 2 || !Path || !Storage ||
        *Storage != "memory-backed")
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "registry_hives mounts require path and storage memory-backed");
    R.push_back({Path->str()});
  }
  return R;
}
llvm::json::Value driverRegistryHivesJSON(
    const std::optional<std::vector<DriverRegistryHive>> &H) {
  if (!H)
    return nullptr;
  llvm::json::Array R;
  for (const auto &Mount : *H)
    R.push_back(
        llvm::json::Object{{"path", Mount.Path}, {"storage", "memory-backed"}});
  return R;
}
} // namespace neverd::emulation
