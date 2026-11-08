//===- X86LegacySystemSelector.cpp - Faultable LDTR/TR loads -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Load protected GDT caches, retaining table faults before publication.
//===----------------------------------------------------------------------===//

#include "X86LegacySystemSelector.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <cstdint>

namespace neverd::translate::detail {

llvm::Expected<X86MemoryAccessResult>
loadLegacySystemSelector(X86LegacyExecutionState &S, X86PagedMemory &M,
                         bool Task, uint16_t Selector) {
  const auto Fault = [](uint8_t Vector, uint32_t Error) {
    X86MemoryAccessResult R;
    R.Status = X86PageWalkStatus::Exception;
    R.Exception = X86MemoryException{Vector, Error, {}};
    return R;
  };
  if (!(Selector & 0xfffc)) {
    if (Task)
      return Fault(13, 0);
    S.Core.LDTR.Selector = Selector;
    S.Core.LDTR.Unusable = true;
    return X86MemoryAccessResult{};
  }
  const uint32_t Index = Selector & 0xfff8;
  if ((Selector & 4) || Index + 7 > S.Core.GDTRLimit)
    return Fault(13, Selector & 0xfffc);
  const uint32_t Address = S.Core.GDTRBase + Index;
  std::array<uint8_t, 8> Bytes{};
  auto Read = M.load(
      {Address, MemoryAccessKind::Read, S.Core.CPL, true, false, true}, Bytes);
  if (!Read || Read->Status != X86PageWalkStatus::Completed)
    return Read;
  uint64_t D = llvm::support::endian::read64le(Bytes.data());
  const unsigned Type = (D >> 40) & 31;
  if (Task ? Type != 1 && Type != 9 : Type != 2)
    return Fault(13, Selector & 0xfffc);
  if (!(D & (uint64_t{1} << 47)))
    return Fault(11, Selector & 0xfffc);
  X86MemoryAccessResult Result;
  if (Task) {
    // The bound profile owns one CPU's entire RAM image. The busy operation
    // retains one write plan and reads its exact fragments before committing;
    // page-walk writes cannot redirect a second walk or be overwritten.
    auto Store = M.prepareStore(
        {Address, MemoryAccessKind::Write, S.Core.CPL, true, false, true}, 8);
    if (!Store)
      return Store.takeError();
    if (Store->Status != X86PageWalkStatus::Completed)
      return static_cast<X86MemoryAccessResult &>(*Store);
    auto Current = M.loadPreparedStore(*Store, Bytes);
    if (!Current || Current->Status != X86PageWalkStatus::Completed)
      return Current;
    Bytes[5] |= 2;
    M.commitStore(*Store, Bytes);
    // A/D updates can alias the descriptor itself. The loaded cache uses
    // the image consumed by the busy operation, not the earlier check image.
    D = llvm::support::endian::read64le(Bytes.data());
    Result = static_cast<X86MemoryAccessResult &>(*Store);
  }
  uint32_t Limit = uint32_t(D & 0xffff) | uint32_t((D >> 32) & 0xf0000);
  if (D & (uint64_t{1} << 55))
    Limit = (Limit << 12) | 0xfff;
  X86LegacySegmentCache Cache{
      uint32_t((D >> 16) & 0xffffff) | uint32_t((D >> 32) & 0xff000000), Limit,
      Selector, uint16_t((D >> 40) & 0xf0ff), false};
  (Task ? S.TR : S.Core.LDTR) = Cache;
  return Result;
}

} // namespace neverd::translate::detail
