//===- AndroidObservation.cpp - Read-only execution evidence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Capture explicit scalar state and memory without patching guest code.
#include "AndroidObservation.h"

#include "AndroidInternal.h"

#include "neverd/emulation/Limits.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <set>

namespace neverd::emulation::android_model {
namespace {
std::optional<uint64_t> registerAddress(uint64_t Base, int64_t Offset) {
  const uint64_t Magnitude =
      Offset < 0 ? 0 - uint64_t(Offset) : uint64_t(Offset);
  if ((Offset < 0 && Base < Magnitude) ||
      (Offset >= 0 && Base > UINT64_MAX - Magnitude))
    return std::nullopt;
  return Offset < 0 ? Base - Magnitude : Base + Magnitude;
}

void recordSnapshotError(NativeMemorySnapshot &Snapshot, llvm::Error E) {
  constexpr size_t Limit =
      ::neverd::limits::emulation::kAndroidFailureMemoryDiagnosticBytes;
  const llvm::StringRef Suffix(diagnostic::SnapshotDiagnosticTruncated);
  static_assert(Limit > sizeof(diagnostic::SnapshotDiagnosticTruncated));
  Snapshot.Bytes.clear();
  Snapshot.Diagnostic = llvm::toString(std::move(E));
  if (Snapshot.Diagnostic.size() > Limit) {
    Snapshot.Diagnostic.resize(Limit - Suffix.size());
    Snapshot.Diagnostic += Suffix;
  }
}
} // namespace

llvm::Expected<uint64_t>
finalMemorySnapshotBytes(const AndroidNativeOptions &Options,
                         uint64_t Available) {
  uint64_t Remaining = Available;
  auto Reserve = [&](uint64_t Size, bool WithDiagnostic) {
    const uint64_t Extra =
        WithDiagnostic
            ? ::neverd::limits::emulation::kAndroidFailureMemoryDiagnosticBytes
            : 0;
    if (!Size || Extra > Remaining || Size > Remaining - Extra)
      return false;
    Remaining -= Size + Extra;
    return true;
  };
  for (const auto &Read : Options.ReadMemory)
    if (Read.Address > UINT64_MAX - Read.Size ||
        !Reserve(Read.Size,
                 Options.ObserveMemoryOnFailure || Options.ObserveMemoryAtStop))
      return failure(diagnostic::SnapshotLimits);
  for (const auto &Read : Options.ReadRegisterMemory)
    if (Read.PC || Read.Register > 31 || !Reserve(Read.Size, true))
      return failure(diagnostic::SnapshotLimits);
  return Available - Remaining;
}

void captureStoppedMemory(ExecutionBackend &CPU,
                          const AndroidNativeOptions &Options,
                          bool IncludeFixed,
                          std::vector<NativeMemorySnapshot> &Out) {
  auto Capture = [&](uint64_t Address, uint64_t Size) {
    auto &Snapshot = Out.emplace_back(Address, std::vector<uint8_t>(Size));
    if (auto E = CPU.snapshotBacking(Address, Snapshot.Bytes))
      recordSnapshotError(Snapshot, std::move(E));
  };
  if (IncludeFixed)
    for (const auto &Read : Options.ReadMemory)
      Capture(Read.Address, Read.Size);
  for (const auto &Read : Options.ReadRegisterMemory) {
    const auto Reg =
        Read.Register == 31
            ? CPURegister::AArch64SP
            : CPURegister(unsigned(CPURegister::AArch64X0) + Read.Register);
    auto Base = CPU.readRegister(Reg);
    auto Address =
        Base ? registerAddress((*Base)[0], Read.Offset) : std::nullopt;
    if (Address) {
      Capture(*Address, Read.Size);
    } else {
      Out.push_back({0, {}, {}, false});
      recordSnapshotError(Out.back(),
                          Base ? failure(diagnostic::SnapshotAddressOverflow)
                               : Base.takeError());
    }
  }
}

llvm::Error
validateExecutionObservation(const NativeExecutionObservationOptions &Options) {
  if (Options.PCs.empty() ||
      Options.PCs.size() >
          ::neverd::limits::emulation::
              kAndroidObservationValidateExecutionObservationOptionsPCsSizeLimit ||
      !Options.Limit)
    return failure(diagnostic::ObservationOptions);
  std::set<uint64_t> Unique;
  for (uint64_t PC : Options.PCs)
    if (PC % 4 || !Unique.insert(PC).second)
      return failure(diagnostic::ObservationOptions);
  for (const auto &Read : Options.ReadMemory)
    if (!Read.Size || Read.Address > UINT64_MAX - Read.Size)
      return failure(diagnostic::SnapshotLimits);
  for (const auto &Read : Options.ReadRegisterMemory)
    if (!Read.Size || Read.Register > 31 ||
        (Read.PC && !Unique.contains(*Read.PC)))
      return failure(diagnostic::ObservationOptions);
  return llvm::Error::success();
}
llvm::Expected<uint64_t>
executionObservationBytes(const NativeExecutionObservationOptions &Options,
                          uint64_t Available) {
  if (auto E = validateExecutionObservation(Options))
    return std::move(E);
  // Every retained record could come from the PC with the largest payload.
  uint64_t Common = 36 * sizeof(uint64_t);
  if (Common > Available)
    return failure(diagnostic::SnapshotLimits);
  auto Reserve = [&](uint64_t &Used, uint64_t Size) {
    if (Available - Used < 16 || Size > Available - Used - 16)
      return false;
    Used += 16 + Size;
    return true;
  };
  for (const auto &Read : Options.ReadMemory)
    if (!Reserve(Common, Read.Size))
      return failure(diagnostic::SnapshotLimits);
  llvm::SmallDenseMap<uint64_t, uint64_t> BytesAtPC;
  uint64_t Largest = 0;
  for (const auto &Read : Options.ReadRegisterMemory) {
    uint64_t &Used = Read.PC ? BytesAtPC[*Read.PC] : Common;
    if (!Reserve(Used, Read.Size))
      return failure(diagnostic::SnapshotLimits);
    if (Read.PC)
      Largest = std::max(Largest, Used);
  }
  if (Largest > Available - Common)
    return failure(diagnostic::SnapshotLimits);
  const uint64_t PerRecord = Common + Largest;
  if (Options.Limit > Available / PerRecord)
    return failure(diagnostic::SnapshotLimits);
  return Options.Limit * PerRecord;
}
llvm::Expected<NativeExecutionObservation> captureExecutionObservation(
    ExecutionBackend &CPU, const NativeExecutionObservationOptions &Options,
    uint64_t PC, uint64_t ThreadID, uint64_t Instructions) {
  NativeExecutionObservation Out;
  Out.PC = PC;
  Out.ThreadID = ThreadID;
  Out.Instructions = Instructions;
  for (unsigned I = 0; I < Out.Registers.size(); ++I) {
    auto R =
        CPU.readRegister(CPURegister(unsigned(CPURegister::AArch64X0) + I));
    if (!R)
      return R.takeError();
    Out.Registers[I] = (*R)[0];
  }
  auto SP = CPU.readRegister(CPURegister::AArch64SP);
  if (!SP)
    return SP.takeError();
  Out.SP = (*SP)[0];
  auto NZCV = CPU.readRegister(CPURegister::AArch64NZCV);
  if (!NZCV)
    return NZCV.takeError();
  Out.NZCV = (*NZCV)[0];
  auto Capture = [&](uint64_t Address, uint64_t Size) -> llvm::Error {
    if (Address > UINT64_MAX - Size)
      return failure(diagnostic::ObservationMemory);
    auto Access =
        CPU.canAccess(Address, Size, emulation::Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return failure(diagnostic::ObservationMemory);
    NativeMemorySnapshot Snapshot{Address, std::vector<uint8_t>(Size)};
    if (auto E = CPU.read(Address, Snapshot.Bytes))
      return std::move(E);
    Out.Memory.push_back(std::move(Snapshot));
    return llvm::Error::success();
  };
  for (const auto &Read : Options.ReadMemory)
    if (auto E = Capture(Read.Address, Read.Size))
      return std::move(E);
  for (const auto &Read : Options.ReadRegisterMemory) {
    if (Read.PC && *Read.PC != PC)
      continue;
    const uint64_t Base =
        Read.Register == 31 ? Out.SP : Out.Registers[Read.Register];
    auto Address = registerAddress(Base, Read.Offset);
    if (!Address)
      return failure(diagnostic::ObservationMemory);
    if (auto E = Capture(*Address, Read.Size))
      return std::move(E);
  }
  return Out;
}
} // namespace neverd::emulation::android_model
