//===- AndroidAllocation.cpp - Bionic heap ownership ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include <algorithm>
#include <array>

namespace neverd::emulation::android_model {
llvm::Expected<uint64_t> Bionic::allocate(uint64_t Size, uint8_t FillByte) {
  uint64_t Effective = std::max(uint64_t(1), Size);
  if (Effective > Options.MemoryLimit ||
      Effective > UINT64_MAX - PageSize + 1) {
    if (auto E = setErrno(linux_model::NoMemory))
      return std::move(E);
    return uint64_t(0);
  }
  uint64_t Mapped = (Effective + PageSize - 1) & ~(PageSize - 1);
  ProcessServiceEvent Event{
      0,
      0,
      {0, Mapped, linux_model::ProtRead | linux_model::ProtWrite,
       linux_model::MapPrivate | linux_model::MapAnonymous, UINT64_MAX, 0},
      std::nullopt};
  auto Address = kernelService(linux_model::ServiceKind::Mmap, Event);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return failure(diagnostic::AllocatorReturn);
  if (**Address >= uint64_t(0) - 4095) {
    if (auto E = setErrno(uint64_t(0) - **Address))
      return std::move(E);
    return uint64_t(0);
  }
  if (FillByte && Size) {
    std::array<uint8_t, PageSize> Bytes;
    Bytes.fill(FillByte);
    for (uint64_t Offset = 0; Offset < Size; Offset += Bytes.size())
      if (auto E =
              CPU.write(**Address + Offset,
                        llvm::ArrayRef(Bytes).take_front(
                            std::min<uint64_t>(Bytes.size(), Size - Offset))))
        return std::move(E);
  }
  Allocations.emplace(**Address, Allocation{Size, Mapped});
  return **Address;
}
llvm::Error Bionic::release(uint64_t Address) {
  if (!Address)
    return llvm::Error::success();
  auto I = Allocations.find(Address);
  if (I == Allocations.end())
    return failure(diagnostic::AllocationOwnership);
  ProcessServiceEvent Event{
      0, 0, {Address, I->second.MappedSize, 0, 0, 0, 0}, std::nullopt};
  auto Returned = kernelService(linux_model::ServiceKind::Munmap, Event);
  if (!Returned)
    return Returned.takeError();
  if (!*Returned || **Returned)
    return failure(diagnostic::AllocationRelease);
  Allocations.erase(I);
  return llvm::Error::success();
}
BionicResult Bionic::malloc(const NativeCallEvent &Call) {
  auto Address = allocate(Call.Arguments[0], Options.Android->MallocFillByte);
  if (!Address)
    return Address.takeError();
  return value(*Address);
}

BionicResult Bionic::calloc(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (A[0] && A[1] > UINT64_MAX / A[0]) {
    if (auto E = setErrno(linux_model::NoMemory))
      return std::move(E);
    return value(0);
  }
  // Anonymous allocations start zeroed.
  auto Address = allocate(A[0] * A[1]);
  if (!Address)
    return Address.takeError();
  return value(*Address);
}

llvm::Expected<uint64_t> Bionic::reallocate(uint64_t Address, uint64_t Size) {
  std::vector<uint8_t> Saved;
  if (Address) {
    auto I = Allocations.find(Address);
    if (I == Allocations.end())
      return failure(diagnostic::ReallocationOwnership);
    uint64_t Copy = std::min(Size, I->second.Size);
    if (auto E = access(Address, Copy, Read))
      return std::move(E);
    Saved.resize(Copy);
    if (Copy)
      if (auto E = CPU.read(Address, Saved))
        return std::move(E);
  }
  auto Resized = allocate(Size, Options.Android->MallocFillByte);
  if (!Resized)
    return Resized.takeError();
  if (!*Resized)
    return uint64_t(0);
  // Preserve the old prefix; the caller-selected fill remains in new bytes.
  if (!Saved.empty())
    if (auto E = CPU.write(*Resized, Saved))
      return std::move(E);
  if (auto E = release(Address))
    return std::move(E);
  return *Resized;
}

BionicResult Bionic::realloc(const NativeCallEvent &Call) {
  auto Address = reallocate(Call.Arguments[0], Call.Arguments[1]);
  if (!Address)
    return Address.takeError();
  return value(*Address);
}

BionicResult Bionic::free(const NativeCallEvent &Call) {
  if (auto E = release(Call.Arguments[0]))
    return std::move(E);
  return value(0); // The ABI leaves x0 unspecified for void calls.
}

} // namespace neverd::emulation::android_model
