//===- KernelModelPlatform.cpp - Explicit guest platform facts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit guest platform facts.
///

#include "KernelModel.h"

namespace neverd::emulation {
llvm::Expected<uint64_t> KernelModel::querySecurePciEnabled() const {
  // The matching 19041 PE observes a zero-argument BOOLEAN result. Its native
  // branches query a live VSM connection and a secure-world operation; neither
  // the static PE bytes nor hypervisor CPUID establish the runtime result.
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "VslGetSecurePciEnabled requires the Windows 10.0.19041 profile");
  if (!SecurePciEnabled.has_value())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "VslGetSecurePciEnabled requires secure_pci_enabled");
  return uint64_t(*SecurePciEnabled);
}
llvm::Expected<uint64_t>
KernelModel::queryDmaGuardPolicy(llvm::ArrayRef<uint64_t> A, bool ZwCall) {
  auto Unsupported = [](const char *Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "DMA Guard policy query: %s", Text);
  };
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return Unsupported("class 202 requires Windows 10.0.19041");
  if (auto Context = executionProcessContext();
      !ZwCall && Context && Context->PreviousMode != 0)
    return Unsupported("Nt class 202 UserMode probing is not modeled");
  const bool ExactLength = uint32_t(A[2]) == 1;
  // Matching ExpQuerySystemInformation checks EXACT length before querying
  // HAL class 0x2f. Only a successful one-byte observation is supplied here;
  // neither static HAL globals nor secure-PCI/CPUID imply this live result.
  if (ExactLength && !DmaGuardPolicyEnabled.has_value())
    return Unsupported("requires dma_guard_policy_enabled");
  auto Writable = [&](uint64_t Address, uint32_t Length) -> llvm::Error {
    if (auto E = objectBuffer(Address, Length, true))
      return E;
    auto Backing = Memory.pinBacking(Address, Length);
    if (!Backing)
      return Backing.takeError();
    return llvm::Error::success();
  };
  // Restrict outputs to ordinary RAM and preflight every store. Invalid guest
  // pointers stop the model without claiming native fault/partial-write rules.
  if (ExactLength)
    if (auto E = Writable(A[1], 1))
      return E;
  if (A[3])
    if (auto E = Writable(A[3], 4))
      return E;
  if (ExactLength)
    if (auto E = Memory.writeInteger(A[1], *DmaGuardPolicyEnabled, 1))
      return E;
  // The native epilogue writes ReturnLength after the one-byte payload, even
  // on length mismatch. Null ReturnLength is allowed; overlap retains order.
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], 1, 4))
      return E;
  return uint64_t(ExactLength ? 0 : 0xc0000004);
}
} // namespace neverd::emulation
