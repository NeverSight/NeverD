//===- RTTI.cpp - Bounded MSVC vftable type evidence ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validates image-relative x64 MSVC RTTI reachable from a primary vftable.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/RTTI.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cstddef>
#include <limits>

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

} // namespace

std::optional<llvm::StringRef>
neverd::coff_loader::readPrimaryVTableTypeName(const BinaryImage &Img,
                                               uint64_t VTable) {
  constexpr size_t PointerSize = sizeof(uint64_t);
  constexpr size_t LocatorSize = 6 * sizeof(uint32_t);
  constexpr size_t HierarchySize = 4 * sizeof(uint32_t);
  constexpr size_t BaseSize = 7 * sizeof(uint32_t);
  constexpr size_t NameOffset = 2 * PointerSize;
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
  const uint64_t TypeVA = Img.rvaToVA(TypeRVA);
  if (!readData(Img, TypeVA, NameOffset + 1, false))
    return std::nullopt;
  const Segment *Seg = Img.getSegmentFor(TypeVA);
  const size_t Offset = static_cast<size_t>(TypeVA - Seg->VA) + NameOffset;
  const size_t Available =
      std::min<uint64_t>(Seg->Data.size(), Seg->Size) - Offset;
  const size_t Bound =
      std::min(Available, limits::kMaxMsvcTypeDescriptorNameBytes);
  const char *Name = reinterpret_cast<const char *>(Seg->Data.data() + Offset);
  const char *End = std::find(Name, Name + Bound, '\0');
  if (End == Name + Bound)
    return std::nullopt;
  llvm::StringRef Type(Name, End - Name);
  if (!(Type.consume_front(".?AV") || Type.consume_front(".?AU")) ||
      !Type.ends_with("@@") || Type.size() <= 2 ||
      llvm::any_of(Type, [](unsigned char C) { return C <= ' ' || C > '~'; }))
    return std::nullopt;
  return Type;
}
