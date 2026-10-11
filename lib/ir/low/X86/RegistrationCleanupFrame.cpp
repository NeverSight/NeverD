//===- RegistrationCleanupFrame.cpp - PE32 cleanup frame coordinates ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate fixed and realigned cleanup prefixes independently of leaves.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

bool RegistrationCleanupRelayABI::matchesParentFrame(
    const RegistrationChainInfo &Chain) const {
  if (!RealignedParent)
    return true;
  return Chain.RealignedFrame && Chain.cxxRuntimeFrameOffset() == 0 &&
         Chain.RealignedFrame->BaseRegister == 6 &&
         Chain.RealignedFrame->BaseOffset == RealignedParent->BaseOffset &&
         Chain.RealignedFrame->SavedParentFrameOffset ==
             RealignedParent->SavedParentFrameOffset;
}

namespace registration_abi {
namespace {
struct FrameInstruction {
  unsigned Size;
  int32_t Displacement;
};

std::optional<FrameInstruction>
readFrameInstruction(const BinaryImage &Image, va_t Address, uint8_t Opcode,
                     uint8_t ModRM, size_t &Work) {
  if (Address > UINT32_MAX - 2 || !chargeCalleeWork(Work, 1))
    return std::nullopt;
  auto Header = readImmutableCodeBytes(Image, Address, 3);
  if (!Header || (*Header)[0] != Opcode ||
      ((*Header)[1] != ModRM && (*Header)[1] != ModRM + 0x40))
    return std::nullopt;
  const unsigned Size = (*Header)[1] == ModRM ? 3 : 6;
  if (Address > uint64_t(UINT32_MAX) + 1 - Size ||
      !chargeCalleeWork(Work, Size))
    return std::nullopt;
  const auto Bytes = readImmutableCodeBytes(Image, Address, Size);
  if (!Bytes)
    return std::nullopt;
  return FrameInstruction{Size, Size == 3 ? int8_t((*Bytes)[2])
                                          : readLE<int32_t>(Bytes->data() + 2)};
}
} // namespace

std::optional<CleanupFramePrefix>
getCleanupFramePrefix(const BinaryImage &Image, va_t Target, size_t &Work) {
  if (Target > UINT32_MAX - 2)
    return std::nullopt;
  const auto Header = readImmutableCodeBytes(Image, Target, 3);
  if (!Header)
    return std::nullopt;
  CleanupFramePrefix Result;
  if ((*Header)[0] != 0x55)
    return Result;
  Result.CallsLeaf = true;
  if (((*Header)[1] == 0x83 || (*Header)[1] == 0x81) && (*Header)[2] == 0xc5) {
    // Fixed LLVM frames: push ebp; add ebp, adjustment.
    Result.Size = (*Header)[1] == 0x83 ? 4 : 7;
    if (Target > uint64_t(UINT32_MAX) + 1 - Result.Size ||
        !chargeCalleeWork(Work, Result.Size))
      return std::nullopt;
    const auto Bytes = readImmutableCodeBytes(Image, Target, Result.Size);
    if (!Bytes)
      return std::nullopt;
    Result.BaseOffset = Result.Size == 4 ? int8_t((*Bytes)[3])
                                         : readLE<int32_t>(Bytes->data() + 3);
    return Result;
  }
  // Realigned LLVM frames: push ebp; lea esi, [ebp+base];
  // mov ebp, [esi+saved]. ESI remains the object's local-frame anchor, while
  // EBP carries the separately saved entry frame. Every called leaf must
  // preserve both; the relay restores runtime EBP before returning.
  const auto Base = readFrameInstruction(Image, Target + 1, 0x8d, 0x75, Work);
  if (!Base)
    return std::nullopt;
  const auto Saved =
      readFrameInstruction(Image, Target + 1 + Base->Size, 0x8b, 0x6e, Work);
  if (!Saved)
    return std::nullopt;
  const int64_t SavedOffset = int64_t(Base->Displacement) + Saved->Displacement;
  if (SavedOffset < INT32_MIN || SavedOffset > INT32_MAX)
    return std::nullopt;
  Result.Size = 1 + Base->Size + Saved->Size;
  Result.BaseRegister = 6;
  Result.BaseOffset = Base->Displacement;
  Result.RealignedParent =
      RegistrationCleanupParentFrame{Result.BaseOffset, int32_t(SavedOffset)};
  return Result;
}
} // namespace registration_abi
} // namespace neverd
