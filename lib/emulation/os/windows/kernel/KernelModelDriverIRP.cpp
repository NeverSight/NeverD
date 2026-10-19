//===- KernelModelDriverIRP.cpp - Caller-allocated IRP storage ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own raw IRP allocations independently of dispatch and completion records.
/// Allocation creates no file, device route or result row. IoFreeIrp releases
/// only packet storage; attached MDLs retain their separate caller ownership.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error driverIRPError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "caller IRP: " + Message);
}
} // namespace

llvm::Error
KernelModel::initializeIRPHeader(uint64_t IRP, uint8_t StackCount,
                                 uint8_t CurrentLocation,
                                 std::array<bool, 16> &IOStatusWritten) {
  if (!StackCount || StackCount > MaxIRPStackCount || !CurrentLocation ||
      CurrentLocation > uint32_t(StackCount) + 1)
    return driverIRPError("header requires a bounded stack and valid cursor");
  struct Field {
    uint64_t Offset;
    uint64_t Value;
    unsigned Width;
  };
  const uint64_t Size = IRPSize + uint64_t(StackCount) * StackSize;
  const uint64_t Stack =
      IRP + IRPSize + uint64_t(CurrentLocation - 1) * StackSize;
  for (const Field &F : std::array<Field, 5>{
           {{IRPTypeOffset, IRPType, sizeof(uint16_t)},
            {ObjectSizeOffset, Size, sizeof(uint16_t)},
            {IRPStackCountOffset, StackCount, sizeof(uint8_t)},
            {IRPLocationOffset, CurrentLocation, sizeof(uint8_t)},
            {IRPStackPointerOffset, Stack, sizeof(uint64_t)}}})
    if (auto E = Memory.writeInteger(IRP + F.Offset, F.Value, F.Width))
      return E;
  if (WindowsIRPInitialization) {
    // Ordinary 19041 IoInitializeIrp and IopAllocateIrpPrivate clear the
    // packet, including the complete IO_STATUS_BLOCK. Seed knowledge here
    // for both producers; adoption must copy it without resetting values.
    // These model writes are not guest CPU stores in DriverResult::Writes.
    constexpr std::array<uint8_t, 16> Zero{};
    if (auto E = Memory.write(IRP + IRPStatusOffset, Zero))
      return E;
    IOStatusWritten.fill(true);
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::allocateDriverIRP(llvm::ArrayRef<uint64_t> Arguments) {
  const uint8_t Count = uint8_t(Arguments[0]);
  if (!Count || Count > MaxIRPStackCount)
    return driverIRPError("IoAllocateIrp requires a positive bounded CCHAR "
                          "stack count");
  if (uint8_t(Arguments[1]))
    return driverIRPError("IoAllocateIrp requires ChargeQuota to be FALSE");
  const uint64_t Size = IRPSize + uint64_t(Count) * StackSize;
  const uint64_t Start =
      (NextAllocation + PoolAlignment - 1) & ~(PoolAlignment - 1);
  if (Start > AllocationEnd || Size > AllocationEnd - Start)
    return 0;
  auto IRP = allocate(Size, PoolAlignment);
  if (!IRP)
    return IRP.takeError();
  DriverIRPAllocation Allocation;
  if (auto E = initializeIRPHeader(*IRP, Count, Count + 1,
                                   Allocation.IOStatusWritten)) {
    FreedRanges.emplace(*IRP, Size);
    return E;
  }
  Allocation.Size = Size;
  Allocation.StackCount = Count;
  auto &Stored = DriverIRPs.emplace(*IRP, Allocation).first->second;
  auto Watch = watchInitialization(*IRP + IRPStatusOffset,
                                   Stored.IOStatusWritten.size());
  if (!Watch)
    return Watch.takeError();
  Stored.IOStatusInitialization = std::move(*Watch);
  return *IRP;
}

llvm::Error KernelModel::releaseDriverIRPStorage(uint64_t IRP) {
  auto Allocation = DriverIRPs.find(IRP);
  if (Allocation == DriverIRPs.end() || Allocation->second.StorageReleased)
    return driverIRPError("release requires live caller-allocated storage");
  if (auto E = prepareReleaseRange(IRP, Allocation->second.Size))
    return E;
  FreedRanges.emplace(IRP, Allocation->second.Size);
  Allocation->second.StorageReleased = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::freeDriverIRP(uint64_t IRP) {
  const auto Allocation = DriverIRPs.find(IRP);
  if (Allocation == DriverIRPs.end())
    return driverIRPError("IoFreeIrp requires the exact base of a live "
                          "caller-allocated IRP");
  if (Allocation->second.Builder)
    return driverIRPError(
        "IoFreeIrp cannot release an I/O-manager-owned synchronous request");
  if (Allocation->second.StorageReleased)
    return driverIRPError("IoFreeIrp cannot release the same packet twice");
  if (Allocation->second.Submitted)
    return freeSubmittedDriverIRP(IRP);
  if (auto E = releaseDriverIRPStorage(IRP))
    return E;
  DriverIRPs.erase(Allocation);
  return llvm::Error::success();
}

} // namespace neverd::emulation
