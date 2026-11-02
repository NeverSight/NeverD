//===- ObjCRelativeLists.cpp - Bounded cache list references -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve each list's image condition while following signed offsets. All
/// entries validate before the caller receives a list or consumes its budget.
//===----------------------------------------------------------------------===//

#include "ObjCRelativeLists.h"

#include "ObjCRuntimeData.h"

#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/MathExtras.h"

namespace neverd::objc {
namespace {

#define NEVERD_OBJC_RELATIVE_LIST_VALUE(Type, Name, Value)                     \
  inline constexpr Type Name = Value;
#define NEVERD_OBJC_RELATIVE_LIST_TEXT(Name, Text)                             \
  inline constexpr char Name[] = Text;
#include "ObjCRelativeLists.def"

llvm::Error fail(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

} // namespace

llvm::Expected<std::vector<ListReference>>
readListReferences(const BinaryImage &Image, va_t Address, size_t &Remaining) {
  std::vector<ListReference> References;
  if (!Address)
    return References;
  if (!(Address & kArrayTag)) {
    References.push_back({Address, std::nullopt});
    return References;
  }
  if (!Image.MachOSharedCache)
    return fail(kMissingCache);
  Address &= ~kArrayTag;
  const RuntimeData Data(Image, PointerView::MetadataTarget);
  if (Address % kArrayAlignment || !Data.bytes(Address, kHeaderBytes))
    return fail(kMalformedList);
  const auto Stride = Data.u32(Address);
  const auto Count = Data.u32(Address + kCountOffset);
  if (!Stride || !Count || *Stride != kEntryBytes || *Count > Remaining)
    return fail(kMalformedList);
  const uint64_t Bytes = kHeaderBytes + uint64_t(*Count) * kEntryBytes;
  const auto *Storage = Data.bytes(Address, Bytes);
  if (!Storage)
    return fail(kMalformedList);
  References.reserve(*Count);
  for (uint32_t I = 0; I < *Count; ++I) {
    const uint64_t Offset = kHeaderBytes + uint64_t(I) * kEntryBytes;
    const uint64_t Word = readLE<uint64_t>(Storage + Offset);
    const uint32_t Index = Word & kImageIndexMask;
    const int64_t Displacement =
        llvm::SignExtend64<kDisplacementBits>(Word >> kImageIndexBits);
    const va_t Slot = Address + Offset;
    const uint64_t Magnitude =
        Displacement < 0 ? uint64_t(-Displacement) : uint64_t(Displacement);
    if (Displacement < 0 ? Magnitude > Slot : Magnitude > InvalidVA - Slot)
      return fail(kMalformedList);
    const va_t Target = Displacement < 0 ? Slot - Magnitude : Slot + Magnitude;
    // An all-zero entry also describes its own empty list header. Real
    // property arrays use this representation for an empty base list.
    if (Target % kListAlignment ||
        (Word && Target >= Address && Target - Address < Bytes) ||
        !Data.bytes(Target, kHeaderBytes))
      return fail(kMalformedList);
    auto Owner = Image.MachOSharedCache->objcImageAddress(Index);
    if (!Owner)
      return Owner.takeError();
    References.push_back({Target, ObjCCacheListOrigin{Index, *Owner}});
  }
  Remaining -= *Count;
  return References;
}

} // namespace neverd::objc
