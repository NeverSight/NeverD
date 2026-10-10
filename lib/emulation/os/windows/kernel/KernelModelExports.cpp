//===- KernelModelExports.cpp - Windows routine lookup -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolves counted guest Unicode routine names without host pointer access.
///
//===----------------------------------------------------------------------===//

#include "KernelAPIIRQL.h"
#include "KernelExportRegistry.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
std::optional<unsigned> KernelModel::halArgumentCount(llvm::StringRef Name) {
#define NEVERD_KERNEL_HAL_API(Symbol, Arity, IRQL, Operation)                  \
  if (Name == #Symbol)                                                         \
    return Arity;
#include "KernelHALAPIs.def"
#undef NEVERD_KERNEL_HAL_API
  return std::nullopt;
}

llvm::Expected<uint64_t> KernelModel::callHAL(llvm::StringRef Name,
                                              llvm::ArrayRef<uint64_t> A) {
  const auto Arity = halArgumentCount(Name);
  if (!DriverObject || !Arity || A.size() != *Arity)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "unknown HAL API, invalid arguments or "
                                   "uninitialized kernel model: " +
                                       Name);
  auto MaximumIRQL = maximumKernelIRQL(Name);
  if (!MaximumIRQL)
    return MaximumIRQL.takeError();
  if (CurrentIRQL > *MaximumIRQL)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   Name + " exceeds its IRQL contract");
#define NEVERD_KERNEL_HAL_API(Symbol, Arity, IRQL, Operation)                  \
  if (Name == #Symbol)                                                         \
    return Operation;
#include "KernelHALAPIs.def"
#undef NEVERD_KERNEL_HAL_API
  llvm_unreachable("validated HAL contract has no dispatch");
}

llvm::Expected<uint64_t>
KernelModel::queryPerformanceCounter(uint64_t FrequencyAddress) {
  // KeQueryPerformanceCounter is available at every valid x64 IRQL. The
  // shared scheduler owns elapsed time; API calls never advance a second
  // clock or invent execution progress in cooperative scheduling mode.
  if (Scheduler.now100ns() > INT64_MAX)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "HAL counter exceeds its signed time domain");
  if (FrequencyAddress) {
    if (auto E = validateGuestAccess(FrequencyAddress, sizeof(uint64_t), true))
      return E;
    constexpr uint64_t Frequency = 1000 * scheduler::TicksPerMillisecond;
    if (auto E =
            Memory.writeInteger(FrequencyAddress, Frequency, sizeof(uint64_t)))
      return E;
  }
  recordUnpackEnvironmentRead();
  return Scheduler.now100ns();
}

llvm::Expected<uint64_t> KernelModel::resolveRoutine(uint64_t Address) {
  if (!Exports)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "kernel export registry is unavailable");
  if (Address > UINT64_MAX - windows::UnicodeRecordSize)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "routine name record overflows");
  if (auto E = validateGuestAccess(Address, windows::UnicodeRecordSize, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + windows::UnicodeMaximumOffset, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + windows::UnicodeBufferOffset, 8);
  if (!Buffer)
    return Buffer.takeError();
  if ((*Length & 1) || *Length > *Maximum ||
      *Length > profile::MaxKernelExportNameSize * 2 ||
      *Buffer > UINT64_MAX - *Length)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid kernel routine UNICODE_STRING");
  if (auto E = validateGuestAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (!Bytes.empty())
    if (auto E = Memory.read(*Buffer, Bytes))
      return E;
  std::string Name;
  for (size_t I = 0; I < Bytes.size(); I += 2) {
    if (Bytes[I + 1] || Bytes[I] < '!' || Bytes[I] > '~')
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unsupported kernel export name encoding");
    Name.push_back(static_cast<char>(Bytes[I]));
  }
  return Exports->resolve(Name);
}
} // namespace neverd::emulation
