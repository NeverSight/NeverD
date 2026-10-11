//===- KernelSystemInformation.cpp - Owned kernel module queries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::querySystemInformation(llvm::ArrayRef<uint64_t> A, bool Trusted) {
  const auto Fail = [](const llvm::Twine &Why) {
    return llvm::createStringError("kernel system information: " + Why);
  };
  if (uint32_t(A[0]) != 11)
    return Fail("unsupported information class");
  if (!Trusted) {
    auto Context = executionProcessContext();
    if (!Context)
      return Fail("Nt query previous-mode is unavailable");
    if (Context->PreviousMode != windows::KernelMode)
      return Fail("Nt queries from user previous-mode require access probing");
  }
  if (LoadedModules.empty())
    return Fail("module inventory is unavailable");
  // Win64 RTL_PROCESS_MODULES has an eight-byte prefix followed by 296-byte
  // records. Public ABI fields are written individually, without host structs.
  constexpr uint32_t Prefix = 8, Record = 296, PathOffset = 40,
                     PathCapacity = 256;
  const uint32_t Required = Prefix + Record * LoadedModules.size();
  const uint32_t Length = A[2];
  const auto CheckOutput = [&](uint64_t Address, uint32_t Size) -> llvm::Error {
    if (auto E = validateGuestAccess(Address, Size, true))
      return E;
    auto Writable = Memory.canAccess(Address, Size, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return Fail("output is not writable");
    return Memory.validateBacking(Address, Size);
  };
  if (Length && Length < Required)
    return Fail("partial module-list output is outside the profile");
  if (A[3])
    if (auto E = CheckOutput(A[3], 4))
      return E;
  if (!Length) {
    if (A[3])
      if (auto E = Memory.writeInteger(A[3], Required, 4))
        return E;
    return uint64_t(0xc0000004u); // STATUS_INFO_LENGTH_MISMATCH
  }
  if (!A[1] || A[1] > UINT64_MAX - Required)
    return Fail("invalid output range");
  if (A[3] && A[3] < A[1] + Required && (A[3] >= A[1] || 4 > A[1] - A[3]))
    return Fail("return length overlaps the module list");
  if (auto E = CheckOutput(A[1], Required))
    return E;
  std::vector<uint8_t> Bytes(Required, 0);
  using namespace llvm::support::endian;
  write32le(Bytes.data(), LoadedModules.size());
  for (size_t I = 0; I < LoadedModules.size(); ++I) {
    const auto &Module = LoadedModules[I];
    // These are paths in the modeled environment, never the host's files.
    const std::string PrefixName = I + 1 == LoadedModules.size()
                                       ? "\\SystemRoot\\System32\\drivers\\"
                                       : "\\SystemRoot\\System32\\";
    const std::string Path = PrefixName + Module.Name;
    if (Module.Name.empty() || Path.size() >= PathCapacity ||
        Module.Size > UINT32_MAX ||
        !std::all_of(Module.Name.begin(), Module.Name.end(),
                     [](unsigned char C) {
                       return C >= 0x20 && C < 0x7f && C != '\\' && C != '/';
                     }))
      return Fail("module has no bounded narrow path or image extent");
    uint8_t *At = Bytes.data() + Prefix + I * Record;
    write64le(At + 16, Module.Base);
    write32le(At + 24, Module.Size);
    write16le(At + 32, I);
    write16le(At + 34, I);
    write16le(At + 36, 1);
    write16le(At + 38, PrefixName.size());
    std::copy(Path.begin(), Path.end(), At + PathOffset);
  }
  if (auto E = Memory.write(A[1], Bytes))
    return E;
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], Required, 4))
      return E;
  // Returning borrowed provider image identities is visible kernel state.
  // Native recovery cannot currently bind such retained data to a fresh OS.
  UnpackOpaqueEffects = true;
  return 0;
}
} // namespace neverd::emulation
