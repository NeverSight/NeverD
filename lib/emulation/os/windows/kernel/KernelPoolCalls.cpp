//===- KernelPoolCalls.cpp - Pool lifetime calls --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
namespace pool {
#define NEVERD_KERNEL_POOL_FLAG(Name, Value) constexpr uint64_t Name = Value;
#include "KernelPoolFlags.def"
#undef NEVERD_KERNEL_POOL_FLAG
} // namespace pool
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::allocatePool(llvm::ArrayRef<uint64_t> A,
                                                   bool Modern) {
  // Dispatch validates each API's exact arity. The original two-argument
  // ExAllocatePool has no caller-supplied tag; keep that absence explicit.
  const bool Tagged = A.size() == 3;
  const uint32_t Tag = Tagged ? static_cast<uint32_t>(A[2]) : 0;
  const uint64_t Flags = Modern ? A[0] : 0;
  const auto AllocationFailure = [&]() -> llvm::Expected<uint64_t> {
    if (Flags & pool::RaiseOnFailure)
      return modelError(
          "POOL_FLAG_RAISE_ON_FAILURE requires guest exception delivery");
    return 0;
  };
  if (Modern) {
    // Optional high bits may be ignored under the documented POOL_FLAGS
    // contract. Required but unmodeled state must never be invented.
    constexpr uint64_t Known = pool::UseQuota | pool::Uninitialized |
                               pool::CacheAligned | pool::RaiseOnFailure |
                               pool::NonPaged | pool::NonPagedExecute |
                               pool::Paged;
    const uint64_t Type =
        Flags & (pool::NonPaged | pool::NonPagedExecute | pool::Paged);
    if (!Tag || !Type || (Type & (Type - 1)) ||
        ((Flags & pool::RequiredMask) & ~Known))
      return AllocationFailure();
    if (Flags & pool::UseQuota)
      return modelError(
          "ExAllocatePool2 quota accounting requires an unmodeled process");
    if (Flags & pool::NonPagedExecute)
      return modelError(
          "ExAllocatePool2 executable pool is outside the data-only arena");
  } else {
    const uint32_t Type = static_cast<uint32_t>(A[0]);
    if (Type != 0 && Type != 1 && Type != PoolNX)
      return modelError("pool model supports NonPagedPool, PagedPool and "
                        "NonPagedPoolNx without additional flags");
    if (Tagged && !Tag)
      return modelError("pool allocation requires non-zero size and tag");
  }
  if (!A[1])
    return modelError("pool allocation requires non-zero size and tag");
  const bool NonPaged = Modern ? (Flags & pool::NonPaged) != 0
                               : static_cast<uint32_t>(A[0]) != PoolPaged;
  if (!NonPaged && CurrentIRQL > APCLevel)
    return modelError("paged pool allocation requires IRQL <= APC_LEVEL");
  uint64_t Alignment = Modern && (Flags & pool::CacheAligned)
                           ? DeviceAlignmentMask + 1
                           : PoolAlignment;
  uint64_t Start = (NextAllocation + Alignment - 1) & ~(Alignment - 1);
  // Allocations smaller than one page never cross a page. Larger allocations
  // are page aligned. A run does not recycle addresses after a free.
  if (A[1] >= profile::PageSize ||
      (Start & (profile::PageSize - 1)) + A[1] > profile::PageSize) {
    Alignment = profile::PageSize;
    Start = (NextAllocation + Alignment - 1) & ~(Alignment - 1);
  }
  if (Start > AllocationEnd || A[1] > AllocationEnd - Start)
    return AllocationFailure();
  auto Pointer = allocatePhysicalBuffer(A[1], Alignment, 0, A[1]);
  if (!Pointer) {
    auto Error = Pointer.takeError();
    if (Error.isA<PhysicalMemoryLimitError>()) {
      llvm::consumeError(std::move(Error));
      return AllocationFailure();
    }
    return std::move(Error);
  }
  // ExAllocatePool2 zeroes by default; old pool and explicit uninitialized
  // allocations use a deterministic concrete byte pattern, never host data.
  if (!Modern || (Flags & pool::Uninitialized))
    if (auto E =
            runtime::writeBytes(Memory, *Pointer, A[1], UninitializedPoolByte))
      return E;
  Allocations.emplace(*Pointer, PoolAllocation{A[1], Tag, NonPaged});
  return *Pointer;
}

llvm::Expected<uint64_t> KernelModel::freePool(llvm::ArrayRef<uint64_t> A,
                                               bool Tagged) {
  if (auto Mdl = MDLs.find(A[0]);
      Mdl != MDLs.end() &&
      (Mdl->second.Owner == LockedMdl::Ownership::AllocatedPages ||
       Mdl->second.Owner == LockedMdl::Ownership::ReleasedPages)) {
    // The WDK ExFreePool macro calls ExFreePoolWithTag with a zero tag.
    if (Tagged && uint32_t(A[1]))
      return modelError("physical MDL descriptors require untagged ExFreePool");
    if (auto E = freeAllocatedMDL(A[0]))
      return E;
    return 0;
  }
  auto It = Allocations.find(A[0]);
  if (It == Allocations.end())
    return modelError("pool free received an unknown or already freed pointer");
  if (Tagged && uint32_t(A[1]) && static_cast<uint32_t>(A[1]) != It->second.Tag)
    return modelError("ExFreePoolWithTag tag does not match allocation");
  if (!It->second.NonPaged && CurrentIRQL > APCLevel)
    return modelError("paged pool free requires IRQL <= APC_LEVEL");
  if (auto E = Physical.canRetire(A[0]))
    return E;
  if (auto E = prepareReleaseRange(A[0], It->second.Size))
    return E;
  if (auto E =
          runtime::writeBytes(Memory, A[0], It->second.Size, FreedPoolByte))
    return E;
  if (auto E = Physical.retire(A[0]))
    return E;
  FreedRanges.emplace(A[0], It->second.Size);
  Allocations.erase(It);
  return 0;
}

} // namespace neverd::emulation
