//===- RTTI.cpp - Bounded MSVC runtime type evidence ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validates image-relative x64 MSVC vftable and record exception metadata.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/RTTI.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

using namespace neverd;

namespace {

const uint8_t *readData(const BinaryImage &Img, uint64_t Address, size_t Size,
                        bool ReadOnly = true) {
  const Segment *Seg = Img.getSegmentFor(Address);
  if (!Seg || !Seg->isReadable() || Seg->isExecutable() ||
      (ReadOnly && Seg->isWritable()) || Size > Seg->Size - (Address - Seg->VA))
    return nullptr;
  return Img.readVA(Address, Size);
}

bool isRecordTypeName(llvm::StringRef Type) {
  return (Type.starts_with("?AV") || Type.starts_with("?AU")) &&
         Type.ends_with("@@") && Type.size() > 5 &&
         llvm::none_of(Type,
                       [](unsigned char C) { return C <= ' ' || C > '~'; });
}

std::optional<llvm::StringRef> readRecordTypeName(const BinaryImage &Img,
                                                  uint64_t Address) {
  constexpr size_t NameOffset = 2 * sizeof(uint64_t);
  if (!readData(Img, Address, NameOffset + 1, false))
    return std::nullopt;
  const Segment *Seg = Img.getSegmentFor(Address);
  const size_t Offset = static_cast<size_t>(Address - Seg->VA) + NameOffset;
  const size_t Available =
      std::min<uint64_t>(Seg->Data.size(), Seg->Size) - Offset;
  const size_t Bound =
      std::min(Available, limits::kMaxMsvcTypeDescriptorNameBytes);
  const char *Name = reinterpret_cast<const char *>(Seg->Data.data() + Offset);
  const char *End = std::find(Name, Name + Bound, '\0');
  if (End == Name + Bound)
    return std::nullopt;
  llvm::StringRef Type(Name, End - Name);
  if (!Type.consume_front(".") || !isRecordTypeName(Type))
    return std::nullopt;
  return Type;
}

} // namespace

std::optional<llvm::StringRef>
neverd::coff_loader::readPrimaryVTableTypeName(const BinaryImage &Img,
                                               uint64_t VTable) {
  constexpr size_t PointerSize = sizeof(uint64_t);
  constexpr size_t LocatorSize = 6 * sizeof(uint32_t);
  constexpr size_t HierarchySize = 4 * sizeof(uint32_t);
  constexpr size_t BaseSize = 7 * sizeof(uint32_t);
  if (!Img.isCOFF() || Img.Arch != Arch::X64 || !Img.is64Bit() ||
      VTable < PointerSize ||
      Img.Base > std::numeric_limits<uint64_t>::max() -
                     std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  const uint8_t *Table = readData(Img, VTable - PointerSize, 2 * PointerSize);
  if (!Table || !Img.isCodeAddress(readLE<uint64_t>(Table + PointerSize)))
    return std::nullopt;
  const uint64_t LocatorVA = readLE<uint64_t>(Table);
  const uint8_t *Locator = readData(Img, LocatorVA, LocatorSize);
  // Version 1 has image-relative pointers and a self RVA. Offset and
  // construction displacement zero restrict this to a primary vftable.
  if (!Locator || readLE<uint32_t>(Locator) != 1 ||
      readLE<uint32_t>(Locator + 4) || readLE<uint32_t>(Locator + 8) ||
      Img.rvaToVA(readLE<uint32_t>(Locator + 20)) != LocatorVA)
    return std::nullopt;
  const uint32_t TypeRVA = readLE<uint32_t>(Locator + 12);
  const uint32_t HierarchyRVA = readLE<uint32_t>(Locator + 16);
  const uint8_t *Hierarchy =
      readData(Img, Img.rvaToVA(HierarchyRVA), HierarchySize);
  if (!TypeRVA || !HierarchyRVA || !Hierarchy ||
      readLE<uint32_t>(Hierarchy) != 0)
    return std::nullopt;
  const uint32_t BaseCount = readLE<uint32_t>(Hierarchy + 8);
  if (!BaseCount)
    return std::nullopt;
  if constexpr (sizeof(size_t) <= sizeof(uint32_t))
    if (BaseCount > std::numeric_limits<size_t>::max() / sizeof(uint32_t))
      return std::nullopt;
  const uint8_t *Bases =
      readData(Img, Img.rvaToVA(readLE<uint32_t>(Hierarchy + 12)),
               size_t(BaseCount) * sizeof(uint32_t));
  if (!Bases)
    return std::nullopt;
  const uint8_t *Self =
      readData(Img, Img.rvaToVA(readLE<uint32_t>(Bases)), BaseSize);
  // The first base describes the complete type, including every remaining
  // base and a nonvirtual displacement of zero. HASPCHD is needed to check
  // the descriptor's hierarchy pointer, rather than reading an older layout.
  constexpr uint32_t HasHierarchy = 0x40;
  if (!Self || readLE<uint32_t>(Self) != TypeRVA ||
      readLE<uint32_t>(Self + 4) != BaseCount - 1 ||
      readLE<uint32_t>(Self + 8) ||
      readLE<uint32_t>(Self + 12) != std::numeric_limits<uint32_t>::max() ||
      readLE<uint32_t>(Self + 16) ||
      !(readLE<uint32_t>(Self + 20) & HasHierarchy) ||
      readLE<uint32_t>(Self + 24) != HierarchyRVA)
    return std::nullopt;
  const auto Type = readRecordTypeName(Img, Img.rvaToVA(TypeRVA));
  return Type ? std::optional(Type->drop_front(3)) : std::nullopt;
}

std::optional<neverd::coff_loader::ThrowInfoType>
neverd::coff_loader::throwInfoTypeName(llvm::StringRef Name) {
  if (!Name.consume_front("_TI"))
    return std::nullopt;
  ThrowInfoType Result;
  if (Name.consume_front("C"))
    Result.Qualifiers |= 1;
  if (Name.consume_front("V"))
    Result.Qualifiers |= 2;
  if (Name.consume_front("U"))
    Result.Qualifiers |= 4;
  if (Name.empty() || Name.front() < '1' || Name.front() > '9' ||
      Name.consumeInteger(10, Result.CatchableCount) ||
      Result.CatchableCount > limits::loader::kMsvcThrowInfoMaxCatchableTypes ||
      !isRecordTypeName(Name))
    return std::nullopt;
  Result.Name = Name;
  return Result;
}

std::optional<neverd::coff_loader::ThrowInfoType>
neverd::coff_loader::readThrowInfoType(const BinaryImage &Img,
                                       uint64_t Address) {
  constexpr size_t ThrowSize = 4 * sizeof(uint32_t);
  constexpr size_t CatchableSize = 7 * sizeof(uint32_t);
  if (!Img.isCOFF() || Img.Arch != Arch::X64 || !Img.is64Bit() ||
      Img.Base > std::numeric_limits<uint64_t>::max() -
                     std::numeric_limits<uint32_t>::max() ||
      Address < Img.Base ||
      Address - Img.Base > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  const uint8_t *Throw = readData(Img, Address, ThrowSize);
  if (!Throw || readLE<uint32_t>(Throw) > 7 || readLE<uint32_t>(Throw + 8))
    return std::nullopt;
  const uint32_t Cleanup = readLE<uint32_t>(Throw + 4);
  if (Cleanup && !Img.isCodeAddress(Img.rvaToVA(Cleanup)))
    return std::nullopt;
  const uint32_t ArrayRVA = readLE<uint32_t>(Throw + 12);
  const uint64_t ArrayVA = Img.rvaToVA(ArrayRVA);
  const uint8_t *Array = readData(Img, ArrayVA, sizeof(uint32_t));
  if (!ArrayRVA || !Array)
    return std::nullopt;
  const uint32_t Count = readLE<uint32_t>(Array);
  if (!Count || Count > limits::loader::kMsvcThrowInfoMaxCatchableTypes)
    return std::nullopt;
  const size_t ArraySize = (size_t(Count) + 1) * sizeof(uint32_t);
  Array = readData(Img, ArrayVA, ArraySize);
  if (!Array)
    return std::nullopt;
  ThrowInfoType Result{{}, readLE<uint32_t>(Throw), Count};
  llvm::SmallVector<std::pair<uint64_t, size_t>, 8> Ranges{
      {Address, ThrowSize}, {ArrayVA, ArraySize}};
  llvm::SmallVector<llvm::StringRef, 4> Names;
  uint64_t TypeVTable = 0;
  uint32_t CompleteSize = 0;
  for (uint32_t I = 0; I < Count; ++I) {
    const uint32_t CatchableRVA = readLE<uint32_t>(Array + (size_t(I) + 1) * 4);
    const uint64_t CatchableVA = Img.rvaToVA(CatchableRVA);
    const uint8_t *Catchable = readData(Img, CatchableVA, CatchableSize);
    // Ordinary records and the standard bad_alloc marker are supported.
    // Scalar, by-reference, virtual-base and unknown property bits are not.
    constexpr uint32_t BadAlloc = 0x10;
    if (!CatchableRVA || !Catchable ||
        (readLE<uint32_t>(Catchable) & ~BadAlloc) ||
        readLE<uint32_t>(Catchable + 12) !=
            std::numeric_limits<uint32_t>::max() ||
        readLE<uint32_t>(Catchable + 16))
      return std::nullopt;
    const uint32_t Offset = readLE<uint32_t>(Catchable + 8);
    const uint32_t Size = readLE<uint32_t>(Catchable + 20);
    const uint32_t Copy = readLE<uint32_t>(Catchable + 24);
    if (!Size || Size > uint32_t(std::numeric_limits<int32_t>::max()) ||
        (Copy && !Img.isCodeAddress(Img.rvaToVA(Copy))))
      return std::nullopt;
    if (I == 0)
      CompleteSize = Size;
    if ((I == 0 && Offset) || Offset > CompleteSize ||
        Size > CompleteSize - Offset)
      return std::nullopt;
    const uint32_t TypeRVA = readLE<uint32_t>(Catchable + 4);
    const uint64_t TypeVA = Img.rvaToVA(TypeRVA);
    const auto Type = readRecordTypeName(Img, TypeVA);
    if (!TypeRVA || !Type || llvm::is_contained(Names, *Type))
      return std::nullopt;
    if ((readLE<uint32_t>(Catchable) & BadAlloc) &&
        *Type != "?AVbad_alloc@std@@")
      return std::nullopt;
    const uint8_t *Descriptor =
        readData(Img, TypeVA, 2 * sizeof(uint64_t), false);
    const uint64_t VTable = readLE<uint64_t>(Descriptor);
    const uint8_t *VTableBytes = readData(Img, VTable, sizeof(uint64_t));
    if (!VTableBytes || !Img.isCodeAddress(readLE<uint64_t>(VTableBytes)) ||
        readLE<uint64_t>(Descriptor + sizeof(uint64_t)) ||
        (I && VTable != TypeVTable))
      return std::nullopt;
    TypeVTable = VTable;
    if (I == 0)
      Result.Name = *Type;
    Names.push_back(*Type);
    Ranges.emplace_back(CatchableVA, CatchableSize);
    // Name excludes the leading dot and terminating zero.
    Ranges.emplace_back(TypeVA, 2 * sizeof(uint64_t) + Type->size() + 2);
  }
  llvm::sort(Ranges);
  for (size_t I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].first - Ranges[I - 1].first < Ranges[I - 1].second)
      return std::nullopt;
  return Result;
}
