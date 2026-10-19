///
/// \file
/// DriverNdisConfiguration.
///

#include "neverd/emulation/DriverNdisConfiguration.h"

#include "neverd/emulation/DriverSession.h"

#include <set>
namespace neverd::emulation {
llvm::Error validateDriverNdisConfiguration(const DriverOptions &O) {
  if (!O.NdisConfiguration)
    return llvm::Error::success();
  auto Invalid = [](const llvm::Twine &T) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "ndis_configuration: " + T);
  };
  if (!O.Registry || !O.Version || O.Version->Major != 10 ||
      O.Version->Minor != 0 || O.Version->Build != 19041)
    return Invalid("requires explicit registry and windows_version 10.0.19041");
  if (O.NdisConfiguration->Bindings.size() > 64)
    return Invalid("exceeds 64 external NDIS bindings");
  if (O.NdisConfiguration->Version &&
      ((*O.NdisConfiguration->Version >> 16) != 6 ||
       (*O.NdisConfiguration->Version & 0xffff) > 99))
    return Invalid("version must describe an observed NDIS 6.x version");
  std::set<uint64_t> Handles;
  for (const auto &B : O.NdisConfiguration->Bindings) {
    if (B.Handle < 0xffff800000000000ULL || (B.Handle & 7) ||
        !Handles.insert(B.Handle).second)
      return Invalid(
          "bindings require distinct aligned canonical kernel identities");
    if (auto E = validateDriverRegistry(
            std::vector<DriverRegistryKey>{{B.RegistryPath, {}}}))
      return Invalid(llvm::toString(std::move(E)));
    const auto Path = llvm::StringRef(B.RegistryPath).lower();
    if (Path == "\\registry\\machine" || Path == "\\registry\\user")
      return Invalid(
          "configuration path must be below a registry namespace root");
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
