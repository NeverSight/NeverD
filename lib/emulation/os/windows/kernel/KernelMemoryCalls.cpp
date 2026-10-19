//===- KernelMemoryCalls.cpp - Checked guest memory -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelAPIKind.h"
#include "KernelException.h"
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

namespace runtime {
llvm::Error checkBufferRange(uint64_t Address, uint64_t Size) {
  if (Size && (!Address || Size - 1 > UINT64_MAX - Address))
    return modelError("invalid or overflowing guest buffer");
  if (Size > profile::KernelArenaSize)
    return modelError("API buffer exceeds the 1 MiB model operation limit");
  return llvm::Error::success();
}
llvm::Error writeBytes(GuestMemory &Memory, uint64_t Address, uint64_t Size,
                       uint8_t Value) {
  if (Size > profile::MaxKernelArenaSize ||
      (Size && (!Address || Size - 1 > UINT64_MAX - Address)))
    return modelError("invalid internal RAM initialization range");
  if (!Size)
    return llvm::Error::success();
  return Memory.write(Address, std::vector<uint8_t>(Size, Value));
}
} // namespace runtime

llvm::Expected<uint64_t>
KernelModel::isResidentKernelAddress(uint64_t Address) const {
  // This is a partial concrete residency query, never an access or ownership
  // grant. Unknown addresses must not acquire a guessed FALSE result.
  if (!Address) {
    const auto Space = Memory.addressSpace();
    if (!Space)
      return modelError("MmIsAddressValid requires address-space authority");
    auto Mappings = Space->mappings();
    if (!Mappings)
      return Mappings.takeError();
    if (!Mappings->empty() && Mappings->front().Address == 0)
      return modelError("MmIsAddressValid does not model mapped zero pages");
    return 0;
  }
  const auto Contains = [Address](uint64_t Base, uint64_t Size) {
    return Address >= Base && Address - Base < Size;
  };
  // Freed bytes can share mapped pages with other live allocations. Physical
  // residency of their enclosing page does not revive their lifetime.
  for (const auto &[Base, Size] : FreedRanges)
    if (Contains(Base, Size))
      return modelError("MmIsAddressValid does not model retired allocations");
  if (Version || DebuggerReference) {
    auto Resident = Memory.canAccess(Address, 1, Read);
    if (!Resident)
      return Resident.takeError();
    if (!*Resident)
      return uint64_t(0);
    auto Storage = Memory.pinBacking(Address, 1);
    if (!Storage)
      return Storage.takeError();
    return uint64_t(1);
  }
  auto Arena = ArenaAllocations.upper_bound(Address);
  if (Arena != ArenaAllocations.begin())
    --Arena;
  if (Arena == ArenaAllocations.end() || !Contains(Arena->first, Arena->second))
    return modelError("MmIsAddressValid requires a live modeled kernel "
                      "allocation without padding");
  if (const auto Pool = Allocations.find(Arena->first);
      Pool != Allocations.end()) {
    if (!Pool->second.NonPaged)
      return modelError("MmIsAddressValid does not model paged pool");
    if (!Contains(Pool->first, Pool->second.Size))
      return modelError("MmIsAddressValid requires exact live pool bytes");
  } else if (Physical.find(Arena->first)) {
    // DMA/MDL/request backing can include page padding outside the exact data
    // owner, and carries its own lifetime. Do not inherit the arena extent.
    return modelError("MmIsAddressValid does not model non-pool physical "
                      "buffer ownership");
  }
  auto Accessible = Memory.canAccess(Address, 1, Read | Write);
  if (!Accessible)
    return Accessible.takeError();
  if (!*Accessible)
    return modelError("MmIsAddressValid requires resident readable and "
                      "writable RAM");
  return 1;
}

llvm::Expected<uint64_t>
KernelModel::checkedMemoryCopy(llvm::ArrayRef<uint64_t> A, bool ClearOnError) {
  // Pinned kernel memcpy_s and memmove_s (RVA 0x3d73f0 / 0x3d7490)
  // both use memmove for successful copies, including overlapping buffers.
  const uint64_t Destination = A[0], Capacity = A[1], Source = A[2],
                 Count = A[3];
  if (!Count)
    return 0;
  if (!Destination)
    return 22;
  if (Source && Capacity >= Count) {
    auto Copied =
        memoryCall(KernelAPIKind::memmove, {Destination, Source, Count});
    if (!Copied)
      return Copied.takeError();
    return 0;
  }
  // Unlike memmove_s, memcpy_s clears the complete supplied capacity. Source
  // validity is not tested on an error branch; the original handler is a RET.
  if (ClearOnError) {
    auto Cleared =
        memoryCall(KernelAPIKind::memset, {Destination, 0, Capacity});
    if (!Cleared)
      return Cleared.takeError();
  }
  return Source ? 34 : 22;
}

llvm::Expected<uint64_t> KernelModel::memoryCall(KernelAPIKind Kind,
                                                 llvm::ArrayRef<uint64_t> A) {
  const bool Zero = Kind == KernelAPIKind::RtlZeroMemory;
  const bool Fill = Kind == KernelAPIKind::RtlFillMemory;
  const uint64_t Size = Zero || Fill ? A[1] : A[2];
  auto PreflightUser = [&](uint64_t Address, bool IsWrite) -> llvm::Error {
    if (!Size || Address >= profile::UserProbeLimit)
      return llvm::Error::success();
    auto Allowed = Memory.canAccess(Address, Size, IsWrite ? Write : Read);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return llvm::make_error<KernelGuestException>(
          exceptions::StatusAccessViolation);
    return llvm::Error::success();
  };
  auto Check = [&](uint64_t Address) -> llvm::Error {
    if (!Version)
      return runtime::checkBufferRange(Address, Size);
    if (Size > KernelMemoryBytes ||
        (Size && (!Address || Size - 1 > UINT64_MAX - Address)))
      return modelError("invalid or oversized native memory operation");
    return llvm::Error::success();
  };
  if (auto E = Check(A[0]))
    return E;
  if (Zero || Fill || Kind == KernelAPIKind::memset) {
    const uint8_t Value = Zero ? 0 : static_cast<uint8_t>(Fill ? A[2] : A[1]);
    if (auto E = validateGuestAccess(A[0], Size, true))
      return E;
    if (auto E = PreflightUser(A[0], true))
      return E;
    if (auto E = runtime::writeBytes(Memory, A[0], Size, Value))
      return E;
    return Kind == KernelAPIKind::memset ? A[0] : 0;
  }
  if (auto E = Check(A[1]))
    return E;
  if ((Kind == KernelAPIKind::memcpy || Kind == KernelAPIKind::RtlCopyMemory) &&
      Size && (A[0] < A[1] ? A[1] - A[0] < Size : A[0] - A[1] < Size))
    return modelError(
        "overlapping buffers passed to a non-overlapping copy API");
  std::vector<uint8_t> Source(Size);
  if (auto E = validateGuestAccess(A[1], Size, false))
    return E;
  if (auto E = PreflightUser(A[1], false))
    return E;
  if (Size)
    if (auto E = Memory.read(A[1], Source))
      return E;
  if (Kind == KernelAPIKind::memcmp ||
      Kind == KernelAPIKind::RtlCompareMemory) {
    std::vector<uint8_t> Other(Size);
    if (auto E = validateGuestAccess(A[0], Size, false))
      return E;
    if (auto E = PreflightUser(A[0], false))
      return E;
    if (Size)
      if (auto E = Memory.read(A[0], Other))
        return E;
    for (size_t I = 0; I < Size; ++I)
      if (Other[I] != Source[I])
        return Kind == KernelAPIKind::RtlCompareMemory
                   ? uint64_t(I)
                   : uint64_t(static_cast<uint32_t>(int(Other[I]) - Source[I]));
    return Kind == KernelAPIKind::RtlCompareMemory ? Size : 0;
  }
  if (auto E = validateGuestAccess(A[0], Size, true))
    return E;
  if (auto E = PreflightUser(A[0], true))
    return E;
  if (Size)
    if (auto E = Memory.write(A[0], Source))
      return E;
  return Kind == KernelAPIKind::memcpy || Kind == KernelAPIKind::memmove ? A[0]
                                                                         : 0;
}

} // namespace neverd::emulation
