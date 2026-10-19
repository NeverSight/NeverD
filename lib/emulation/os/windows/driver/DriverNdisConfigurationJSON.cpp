///
/// \file
/// DriverNdisConfigurationJSON.
///

#include "DriverNdisConfigurationJSON.h"
namespace neverd::emulation {
llvm::Expected<DriverNdisConfiguration>
parseDriverNdisConfiguration(const llvm::json::Value &V) {
  auto Invalid = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "ndis_configuration requires bindings with handle/registry_path and "
        "optional integer version");
  };
  const auto *O = V.getAsObject();
  if (!O || O->size() > 2 || !O->getArray("bindings"))
    return Invalid();
  for (const auto &[K, V] : *O)
    if (K != "bindings" && K != "version")
      return Invalid();
  DriverNdisConfiguration R;
  if (const auto *V = O->get("version")) {
    auto N = V->getAsUINT64();
    if (!N || *N > UINT32_MAX)
      return Invalid();
    R.Version = uint32_t(*N);
  }
  const auto &Bindings = *O->getArray("bindings");
  if (Bindings.size() > 64)
    return Invalid();
  for (const auto &V : Bindings) {
    const auto *B = V.getAsObject();
    if (!B || B->size() != 2 || !B->get("handle"))
      return Invalid();
    const auto N = B->get("handle")->getAsUINT64();
    const auto P = B->getString("registry_path");
    if (!N || !P)
      return Invalid();
    R.Bindings.push_back({*N, P->str()});
  }
  return R;
}
llvm::json::Value
driverNdisConfigurationJSON(const DriverNdisConfiguration &C) {
  llvm::json::Array Bindings;
  for (const auto &B : C.Bindings)
    Bindings.push_back(llvm::json::Object{{"handle", B.Handle},
                                          {"registry_path", B.RegistryPath}});
  llvm::json::Object R{{"bindings", std::move(Bindings)}};
  if (C.Version)
    R["version"] = uint64_t(*C.Version);
  return R;
}
} // namespace neverd::emulation
