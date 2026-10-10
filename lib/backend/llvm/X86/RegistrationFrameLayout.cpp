//===- RegistrationFrameLayout.cpp - Physical PE32 frame coordinates -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {

std::optional<int64_t> alignX86RegistrationOffset(int64_t Offset,
                                                  uint64_t RootAlignment,
                                                  uint32_t Mask) {
  const uint32_t Alignment = uint32_t(0) - Mask;
  if (!llvm::isPowerOf2_64(RootAlignment) || !llvm::isPowerOf2_32(Alignment) ||
      Alignment > RootAlignment || Offset < INT32_MIN || Offset > INT32_MAX)
    return std::nullopt;
  return llvm::APInt(32, uint32_t(Offset) & Mask).getSExtValue();
}

std::optional<uint32_t>
X86RegistrationFrameLayout::runtimeOffset(int32_t Offset,
                                          uint64_t Width) const {
  const int64_t Address = int64_t(Establisher) + Offset;
  if (Address < 0 || uint64_t(Address) > Size || Width > Size - Address)
    return std::nullopt;
  return uint32_t(Address);
}

std::optional<X86RegistrationFrameLayout>
projectX86RegistrationFrame(const RegistrationFrameCoordinate &Coordinate,
                            uint64_t Size, uint64_t EntrySP,
                            uint64_t Alignment) {
  if (!Size || Size > INT32_MAX || EntrySP > Size || EntrySP < 20 ||
      !llvm::isPowerOf2_64(Alignment) || !Coordinate.Alignment)
    return std::nullopt;
  auto Aligned =
      alignX86RegistrationOffset(int64_t(EntrySP) + Coordinate.EntryOffset,
                                 Alignment, uint32_t(0) - Coordinate.Alignment);
  if (!Aligned)
    return std::nullopt;
  const int64_t Establisher = *Aligned + Coordinate.AlignedOffset;
  if (Establisher < 20 || Establisher >= int64_t(Size))
    return std::nullopt;
  return X86RegistrationFrameLayout{uint32_t(Size), uint32_t(EntrySP),
                                    uint32_t(Establisher)};
}

} // namespace neverd
