//===- KernelModelSystemInformation.cpp - Guest loaded-module queries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest loaded-module queries.
///

#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation {
void KernelModel::addReferenceModule(uint64_t Base, uint64_t Size) {
  LoadedModules.insert(
      LoadedModules.begin(),
      {Base, Size, "\\SystemRoot\\system32\\ntoskrnl.exe", false});
}
llvm::Expected<uint64_t>
KernelModel::queryNativeSystemInformation(llvm::ArrayRef<uint64_t> A,
                                          bool ZwCall) {
  if (uint32_t(A[0]) == 5)
    return queryProcessInformation(A, ZwCall);
  if (uint32_t(A[0]) == 76)
    return queryFirmwareInformation(A, ZwCall);
  if (uint32_t(A[0]) == 202)
    return queryDmaGuardPolicy(A, ZwCall);
  if (uint32_t(A[0]) == 103)
    return queryCodeIntegrity(A, ZwCall);
  // SystemModuleInformation, x64 RTL_PROCESS_MODULES/INFORMATION layout:
  // https://github.com/winsiderss/phnt/blob/master/ntldr.h
  constexpr uint64_t Header = 8, Record = 296, PathOffset = 40;
  constexpr uint32_t LengthMismatch = 0xc0000004;
  const uint32_t Class = uint32_t(A[0]);
  if (Class != 11 && Class != 196)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "unmodeled system information class: %u",
                                   uint32_t(A[0]));
  const uint64_t Required =
      Class == 196 ? 4 : Header + Record * LoadedModules.size();
  auto Writable = [&](uint64_t Address, uint64_t Length) -> llvm::Error {
    if (auto E = runtime::checkBufferRange(Address, Length))
      return E;
    if (auto E = validateGuestAccess(Address, Length, true))
      return E;
    auto Allowed = Memory.canAccess(Address, Length, Write);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "system information output is not writable");
    return llvm::Error::success();
  };
  if (A[3])
    if (auto E = Writable(A[3], 4))
      return E;
  if (uint32_t(A[2]) < Required) {
    if (A[3])
      if (auto E = Memory.writeInteger(A[3], Required, 4))
        return E;
    return uint64_t(LengthMismatch);
  }
  if (auto E = Writable(A[1], Required))
    return E;
  std::vector<uint8_t> Bytes(Required, 0);
  if (Class == 196) {
    llvm::support::endian::write32le(
        Bytes.data(), uint32_t(windows::KernelVAShadowInformationFlags));
    // SYSTEM_KERNEL_VA_SHADOW_INFORMATION: this model has no Windows KVA
    // shadow address-space switching or host speculative-feature assertions.
    // All flags are clear, independently of the host's mitigations.
    if (auto E = Memory.write(A[1], Bytes))
      return E;
    if (A[3])
      if (auto E = Memory.writeInteger(A[3], Required, 4))
        return E;
    return uint64_t(windows::StatusSuccess);
  }
  llvm::support::endian::write32le(Bytes.data(), LoadedModules.size());
  for (size_t I = 0; I < LoadedModules.size(); ++I) {
    const auto &Module = LoadedModules[I];
    if (Module.Path.size() >= 256 || Module.Size > UINT32_MAX)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "loaded module exceeds query layout");
    auto *Out = Bytes.data() + Header + I * Record;
    llvm::support::endian::write64le(Out + 16, Module.Base);
    llvm::support::endian::write32le(Out + 24, Module.Size);
    if (Module.Guest) {
      auto Flags = Memory.readInteger(
          (Module.Record ? Module.Record : DriverModule) + 0x68, 4);
      if (!Flags)
        return Flags.takeError();
      llvm::support::endian::write32le(Out + 28, *Flags);
    }
    llvm::support::endian::write16le(Out + 32, I);
    llvm::support::endian::write16le(Out + 34, I);
    llvm::support::endian::write16le(Out + 36, 1);
    llvm::support::endian::write16le(Out + 38,
                                     Module.Path.find_last_of('\\') + 1);
    std::copy(Module.Path.begin(), Module.Path.end(), Out + PathOffset);
  }
  if (auto E = Memory.write(A[1], Bytes))
    return E;
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], Required, 4))
      return E;
  return uint64_t(windows::StatusSuccess);
}
} // namespace neverd::emulation
