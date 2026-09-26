//===- KernelModelPhysicalMemory.cpp - Existing RAM physical ownership ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register exact data ranges when existing model allocations are created.
/// MDLs describe those allocations and publish the same physical page IDs.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

namespace neverd::emulation {
namespace {
llvm::Error physicalModelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::allocatePhysicalBuffer(uint64_t AllocationSize, uint64_t Alignment,
                                    uint64_t DataOffset, uint64_t DataSize) {
  if (!AllocationSize || AllocationSize > profile::KernelArenaSize ||
      !Alignment || (Alignment & (Alignment - 1)) || !DataSize ||
      DataOffset >= AllocationSize || DataSize > AllocationSize - DataOffset ||
      NextAllocation > UINT64_MAX - (Alignment - 1))
    return physicalModelError("invalid physical data allocation range");
  const uint64_t Start = (NextAllocation + Alignment - 1) & ~(Alignment - 1);
  if (Start > AllocationEnd || AllocationSize > AllocationEnd - Start)
    return physicalModelError("Windows initialization model arena exhausted");
  // Validate every new page and the exact exposed data range before allocating
  // or zeroing anything. Page padding remains outside this owner's authority.
  if (auto E = Physical.canRegisterRegion(Start, Start + DataOffset, DataSize))
    return std::move(E);
  auto Address = allocate(AllocationSize, Alignment);
  if (!Address)
    return Address.takeError();
  if (auto E =
          Physical.registerRegion(*Address, *Address + DataOffset, DataSize))
    return std::move(E);
  return *Address;
}

llvm::Error KernelModel::initializeMDLPhysicalPages(const LockedMdl &State) {
  const uint64_t Backing = State.BackingAddress;
  auto Owner = Physical.ownerForRange(Backing, State.ByteCount);
  if (!Owner)
    return Owner.takeError();
  const auto *Region = Physical.find(*Owner);
  auto Segments =
      Physical.describe(*Owner, Backing - Region->Backing, State.ByteCount);
  if (!Segments)
    return Segments.takeError();
  const uint64_t Count = ((Backing & (profile::PageSize - 1)) +
                          State.ByteCount + profile::PageSize - 1) /
                         profile::PageSize;
  if (Segments->size() != Count ||
      State.Size < windows::MDLSize + Count * profile::PointerSize)
    return physicalModelError(
        "MDL physical pages do not match descriptor extent");
  auto Writable = Memory.canAccess(State.Address, State.Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return physicalModelError("MDL physical pages require writable descriptor "
                              "storage");
  for (size_t I = 0; I != Segments->size(); ++I)
    if (auto E = Memory.writeInteger(
            State.Address + windows::MDLSize + I * profile::PointerSize,
            (*Segments)[I].Physical / profile::PageSize, profile::PointerSize))
      return E;
  return llvm::Error::success();
}
} // namespace neverd::emulation
