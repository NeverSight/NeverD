//===- COFFRegistrationEHThrowInfo.cpp - Checked PE32 trivial throws -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>

namespace neverd::coff_loader {

std::optional<X86SimpleCxxThrowInfo>
getCheckedX86SimpleCxxThrowInfo(const BinaryImage &Img, va_t Address) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF)
    return std::nullopt;
  X86SimpleCxxThrowInfo Result;
  Result.Address = Address;
  auto ReadImmutable = [&](va_t VA, uint32_t Size) -> const uint8_t * {
    if (!VA || VA > UINT32_MAX || Size > uint64_t(UINT32_MAX) + 1 - VA)
      return nullptr;
    const auto *Owner = Img.getSegmentFor(VA);
    const auto *Bytes = Img.readVA(VA, Size);
    if (!Owner || !Owner->isReadable() || Owner->isWritable() ||
        Owner->isExecutable() || !Bytes)
      return nullptr;
    Result.ReadOnlyRanges.push_back({VA, VA + Size});
    return Bytes;
  };
  const auto *Throw = ReadImmutable(Address, 16);
  if (!Throw || (readLE<uint32_t>(Throw) & ~7u) ||
      readLE<uint32_t>(Throw + 4) || readLE<uint32_t>(Throw + 8))
    return std::nullopt;
  Result.Attributes = readLE<uint32_t>(Throw);
  const auto *Array = ReadImmutable(readLE<uint32_t>(Throw + 12), 8);
  if (!Array || readLE<uint32_t>(Array) != 1)
    return std::nullopt;
  const auto *Catchable = ReadImmutable(readLE<uint32_t>(Array + 4), 28);
  // CT_IsSimpleType selects a scalar/pointer copy. With no flags and no copy
  // routine, the CRT copies an object representation instead. Both paths are
  // bounded by the same size and identity adjustment below. Reference-only,
  // virtual-base and managed conversions require separate contracts.
  if (!Catchable || (readLE<uint32_t>(Catchable) & ~1u) ||
      readLE<int32_t>(Catchable + 8) != 0 ||
      readLE<int32_t>(Catchable + 12) != -1 ||
      readLE<int32_t>(Catchable + 16) != 0 ||
      readLE<uint32_t>(Catchable + 24) != 0)
    return std::nullopt;
  Result.ObjectSize = readLE<uint32_t>(Catchable + 20);
  Result.TypeDescriptorVA = readLE<uint32_t>(Catchable + 4);
  if (!Result.ObjectSize ||
      Result.ObjectSize > limits::kMaxRegistrationEHStateWork ||
      !Result.TypeDescriptorVA || Result.TypeDescriptorVA > UINT32_MAX - 8 ||
      !Img.readVA(Result.TypeDescriptorVA, 9))
    return std::nullopt;
  // MSVC TypeDescriptor includes two pointer words followed by a bounded
  // decorated name. Its cache word may be writable; it is not an object-size
  // authority and must not be conflated with the immutable ThrowInfo graph.
  constexpr uint32_t MaxTypeName = 4096;
  bool Terminated = false;
  for (uint32_t Index = 0; Index != MaxTypeName; ++Index) {
    const uint64_t VA = uint64_t(Result.TypeDescriptorVA) + 8 + Index;
    if (VA > UINT32_MAX)
      return std::nullopt;
    const auto *Byte = Img.readVA(VA, 1);
    const auto *Owner = Img.getSegmentFor(VA);
    if (!Byte || !Owner || !Owner->isReadable())
      return std::nullopt;
    if (Index == 0 && *Byte != '.')
      return std::nullopt;
    if (*Byte == 0) {
      Result.TypeDescriptorRange = {Result.TypeDescriptorVA, VA + 1};
      Terminated = true;
      break;
    }
  }
  if (!Terminated)
    return std::nullopt;
  std::sort(Result.ReadOnlyRanges.begin(), Result.ReadOnlyRanges.end(),
            [](const auto &A, const auto &B) { return A.Begin < B.Begin; });
  for (size_t I = 1; I != Result.ReadOnlyRanges.size(); ++I)
    if (Result.ReadOnlyRanges[I - 1].overlaps(Result.ReadOnlyRanges[I]))
      return std::nullopt;
  for (const auto &Range : Result.ReadOnlyRanges)
    if (Range.overlaps(Result.TypeDescriptorRange))
      return std::nullopt;
  return Result;
}

} // namespace neverd::coff_loader
