///
/// \file
/// DriverCPUIDJSON.
///

#include "DriverCPUIDJSON.h"
namespace neverd::emulation {
llvm::Expected<DriverCPUIDPolicy>
parseDriverCPUIDPolicy(const llvm::json::Value &V) {
  auto Invalid = []() -> llvm::Error {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "cpuid_policy requires known invalid_leaf/hypervisor policy strings");
  };
  const auto *O = V.getAsObject();
  if (!O)
    return Invalid();
  for (const auto &KV : *O)
    if (llvm::StringRef(KV.first) != "invalid_leaf" &&
        llvm::StringRef(KV.first) != "hypervisor")
      return Invalid();
  DriverCPUIDPolicy P;
  if (const auto *F = O->get("invalid_leaf")) {
    auto S = F->getAsString();
    if (S == "intel-max-basic-v1")
      P.InvalidLeaf = DriverCPUIDInvalidLeafPolicy::IntelMaxBasicV1;
    else if (S != "strict")
      return Invalid();
  }
  if (const auto *F = O->get("hypervisor")) {
    auto S = F->getAsString();
    if (S == "neverd-v1")
      P.Hypervisor = DriverCPUIDHypervisorPolicy::NeverDV1;
    else if (S != "observed")
      return Invalid();
  }
  return P;
}
llvm::json::Object driverCPUIDPolicyJSON(const DriverCPUIDPolicy &P) {
  const char *Invalid = "invalid", *Hypervisor = "invalid";
  switch (P.InvalidLeaf) {
  case DriverCPUIDInvalidLeafPolicy::Strict:
    Invalid = "strict";
    break;
  case DriverCPUIDInvalidLeafPolicy::IntelMaxBasicV1:
    Invalid = "intel-max-basic-v1";
    break;
  }
  switch (P.Hypervisor) {
  case DriverCPUIDHypervisorPolicy::Observed:
    Hypervisor = "observed";
    break;
  case DriverCPUIDHypervisorPolicy::NeverDV1:
    Hypervisor = "neverd-v1";
    break;
  }
  return llvm::json::Object{{"invalid_leaf", Invalid},
                            {"hypervisor", Hypervisor}};
}
} // namespace neverd::emulation
