//===- ARMEHABIIndex.cpp - .ARM.exidx index table location ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Locates the `.ARM.exidx` index sections of an image, reads the bytes each
/// one is backed by, and answers the addressing questions the index itself
/// leaves open: how a `prel31` field resolves, and where the code covered by
/// the last entry stops.
///
//===----------------------------------------------------------------------===//

#include "ARMEHABIDetail.h"

#include "neverd/object/SectionNames.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace neverd::arm_ehabi {
namespace detail {
namespace {

/// Bytes of \p Sec, preferring the section's own copy and falling back to the
/// segment it was mapped into.  A section that survives only as a mapping
/// still has to be readable: an image loaded from memory has no section bytes
/// at all.
bool readSection(const BinaryImage &Img, const Section &Sec,
                 TableSection &Out) {
  if (Sec.Size == 0)
    return false;
  if (!Sec.Data.empty()) {
    Out.Data = Sec.Data.data();
    Out.Size = std::min<size_t>(Sec.Data.size(), static_cast<size_t>(Sec.Size));
    Out.VA = Sec.VA;
    return Out.Size >= kIndexEntrySize;
  }
  if (Sec.VA == 0 || Sec.Size > std::numeric_limits<size_t>::max())
    return false;
  const uint8_t *Bytes = Img.readVA(Sec.VA, static_cast<size_t>(Sec.Size));
  if (!Bytes)
    return false;
  Out.Data = Bytes;
  Out.Size = static_cast<size_t>(Sec.Size);
  Out.VA = Sec.VA;
  return Out.Size >= kIndexEntrySize;
}

} // namespace

va_t resolvePrel31(uint32_t Word, va_t FieldVA) {
  int32_t Displacement = static_cast<int32_t>(Word << 1) >> 1;
  return static_cast<va_t>(
      (static_cast<uint64_t>(FieldVA) + static_cast<uint64_t>(Displacement)) &
      0xFFFFFFFFull);
}

std::vector<TableSection> findIndexSections(const BinaryImage &Img) {
  std::vector<TableSection> Result;
  for (const Section &Sec : Img.Sections) {
    llvm::StringRef Name(Sec.Name);
    if (Sec.Type != llvm::ELF::SHT_ARM_EXIDX &&
        Name != section_names::elf::ArmExIdx &&
        !Name.starts_with(section_names::elf::ArmExIdxPrefix))
      continue;
    TableSection Table;
    if (readSection(Img, Sec, Table))
      Result.push_back(Table);
  }
  std::sort(
      Result.begin(), Result.end(),
      [](const TableSection &A, const TableSection &B) { return A.VA < B.VA; });
  return Result;
}

va_t executableEndFor(const BinaryImage &Img, va_t Address) {
  va_t End = 0;
  for (const Section &Sec : Img.Sections) {
    if (Sec.Size == 0 || Address < Sec.VA || Address >= Sec.VA + Sec.Size)
      continue;
    const Segment *Seg = Img.getSegmentFor(Sec.VA);
    if (!Seg || !Seg->isExecutable())
      continue;
    End = static_cast<va_t>(Sec.VA + Sec.Size);
    break;
  }
  if (End != 0)
    return End;
  if (const Segment *Seg = Img.getSegmentFor(Address);
      Seg && Seg->isExecutable())
    return static_cast<va_t>(Seg->VA + Seg->Size);
  return 0;
}

} // namespace detail
} // namespace neverd::arm_ehabi
