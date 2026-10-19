//===- KernelSystemInformation.cpp - Owned kernel module queries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "KernelVaShadowQuery.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Expected<uint64_t> KernelModel::pcToFileHeader(uint64_t PC,
                                                     uint64_t Output) {
  if (LoadedModules.empty() || (!Exports && !DriverModule && !Version))
    return llvm::createStringError("kernel module inventory is unavailable");
  std::optional<uint64_t> Base;
  for (const auto &Module : LoadedModules) {
    if (!Module.Size || Module.Size > UINT32_MAX ||
        Module.Size > UINT64_MAX - Module.Base)
      return llvm::createStringError("kernel PC lookup requires bounded module "
                                     "image extents");
    if (PC < Module.Base || PC - Module.Base >= Module.Size)
      continue;
    if (Base && *Base != Module.Base)
      return llvm::createStringError("kernel PC lookup has ambiguous module "
                                     "image ownership");
    Base = Module.Base;
  }
  // PC is a value, not a memory read or an executable-address check. Module
  // headers, image holes and service gates use the same inventory as queries.
  // Dynamic unwind/retpoline ranges are not part of the fixed module profile.
  if (auto E = validateGuestAccess(Output, 8, true))
    return E;
  const uint64_t Value = Base.value_or(0);
  if (auto E = Memory.writeInteger(Output, Value, 8))
    return E;
  return Value;
}

llvm::Expected<uint64_t>
KernelModel::querySystemInformation(llvm::ArrayRef<uint64_t> A, bool Trusted) {
  const auto Fail = [](const llvm::Twine &Why) {
    return llvm::createStringError("kernel system information: " + Why);
  };
  const uint32_t Class = A[0];
  if ((Version || DriverModule || Class == 5 || Class == 76 || Class == 103 ||
       Class == 202) &&
      !(Class == kernel_va_shadow_query::InformationClass &&
        KernelVaShadowQuery))
    return queryNativeSystemInformation(A, Trusted);
  if (Class == 11 && !Exports && !DriverModule && !Version)
    return Fail("module inventory is unavailable");
  if (Class != 11 && Class != kernel_va_shadow_query::InformationClass)
    return Fail("unsupported information class");
  if (!Trusted) {
    auto Context = executionProcessContext();
    if (!Context)
      return Fail("Nt query previous-mode is unavailable");
    if (Context->PreviousMode != windows::KernelMode)
      return Fail("Nt queries from user previous-mode require access probing");
  }
  if (Class == kernel_va_shadow_query::InformationClass)
    return queryKernelVaShadow(A);
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
