//===- DataInCode.cpp - Mach-O embedded data ranges -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "DataInCode.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/EmbeddedRead.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"

namespace neverd {
namespace {
#define MACHO_DICE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "DataInCode.def"
} // namespace

llvm::Expected<std::vector<MachODataInCodeRange>>
decodeMachODataInCode(llvm::ArrayRef<uint8_t> Bytes, va_t Base,
                      llvm::endianness Order) {
  using Entry = llvm::MachO::data_in_code_entry;
  namespace endian = llvm::support::endian;
  if (Bytes.size() % sizeof(Entry))
    return llvm::createStringError(llvm::errc::invalid_argument, kRecordSize);
  std::vector<MachODataInCodeRange> Result;
  Result.reserve(Bytes.size() / sizeof(Entry));
  while (!Bytes.empty()) {
    const auto *P = Bytes.data();
    const auto Offset = endian::read32(P + offsetof(Entry, offset), Order);
    const auto Length = endian::read16(P + offsetof(Entry, length), Order);
    const auto Kind = endian::read16(P + offsetof(Entry, kind), Order);
    if (!Length || Offset > InvalidVA - Base ||
        Length > InvalidVA - Base - Offset)
      return llvm::createStringError(llvm::errc::invalid_argument, kRange);
    Result.push_back({Base + Offset, Base + Offset + Length, Kind});
    Bytes = Bytes.drop_front(sizeof(Entry));
  }
  llvm::sort(Result,
             [](const auto &L, const auto &R) { return L.Begin < R.Begin; });
  if (std::adjacent_find(Result.begin(), Result.end(),
                         [](const auto &L, const auto &R) {
                           return L.End > R.Begin;
                         }) != Result.end())
    return llvm::createStringError(llvm::errc::invalid_argument, kOverlap);
  return Result;
}
bool coverMachODataInCodeRead(llvm::ArrayRef<MachODataInCodeRange> Records,
                              va_t Address, uint64_t Size, uint16_t Kind,
                              bool AllowUnmarked,
                              std::vector<MachODataInCodeRange> *Missing) {
  if (!Size || Size > InvalidVA - Address)
    return false;
  const auto Overlap = machODataInCodeRanges(Records, Address, Size);
  if (llvm::any_of(Overlap, [=](const auto &R) { return R.Kind != Kind; }))
    return false;
  if (AllowUnmarked && !Missing)
    return true;

  va_t Next = Address;
  for (const auto &R : Overlap) {
    if (Next < R.Begin) {
      if (!AllowUnmarked)
        return false;
      Missing->push_back({Next, R.Begin, Kind});
    }
    Next = R.End;
  }
  if (Next < Address + Size) {
    if (!AllowUnmarked)
      return false;
    Missing->push_back({Next, Address + Size, Kind});
  }
  return true;
}

bool MachOBoundedTableStorage::contains(va_t Address, uint64_t Size) const {
  return Address >= Base && Address < End && Size <= End - Address &&
         coverMachODataInCodeRead(Records, Address, Size, Kind, AllowUnmarked);
}

std::optional<MachOBoundedTableStorage>
getMachOBoundedTableStorage(const BinaryImage &Image, va_t Base,
                            uint16_t EntrySize) {
  if (!Image.isArm64e() || Image.MachODataInCode.empty())
    return std::nullopt;
  uint16_t Kind;
  switch (EntrySize) {
  case sizeof(uint8_t):
    Kind = llvm::MachO::DICE_KIND_JUMP_TABLE8;
    break;
  case sizeof(uint16_t):
    Kind = llvm::MachO::DICE_KIND_JUMP_TABLE16;
    break;
  case sizeof(uint32_t):
    Kind = llvm::MachO::DICE_KIND_JUMP_TABLE32;
    break;
  default:
    return std::nullopt;
  }
  const auto *Seg = Image.getSegmentFor(Base);
  if (!Seg || !Seg->isExecutable() || Seg->isWritable() ||
      Seg->Data.size() > InvalidVA - Seg->VA)
    return std::nullopt;
  va_t End = Seg->VA + Seg->Data.size();
  bool AllowUnmarked = true;
  if (const auto *Sec = Image.getSectionFor(Base)) {
    if (!(Sec->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                       llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) ||
        Sec->isWritable())
      return std::nullopt;
    const auto Size = std::min(Sec->Size, Sec->FileSz);
    if (Size > InvalidVA - Sec->VA)
      return std::nullopt;
    End = std::min(End, Sec->VA + Size);
  } else if (Image.segmentHasReadableSectionMetadata(*Seg)) {
    // Appended code can lie outside __text inside a segment with sections.
    // Only explicit records own such storage. Stay on this side of the next
    // section even if its first record happens to have the same kind.
    AllowUnmarked = false;
    for (const auto &Sec : Image.Sections)
      if (Sec.Size && Sec.VA > Base)
        End = std::min(End, Sec.VA);
  }
  return MachOBoundedTableStorage{Base, End, Kind, AllowUnmarked,
                                  Image.MachODataInCode};
}

llvm::Error loadMachODataInCode(BinaryImage &Img,
                                const llvm::MachO::linkedit_data_command &Dice,
                                llvm::endianness Order) {
  if (!rangeInBounds(Dice.dataoff, Dice.datasize, Img.Raw.size()))
    return llvm::createStringError(llvm::errc::invalid_argument, kRange);
  const bool Packed = bool(Img.MachOSharedCache);
  const va_t Base = Img.IsRelocatable || Packed ? 0 : Img.fileOffsetToVA(0);
  auto Ranges = decodeMachODataInCode(
      llvm::ArrayRef(Img.Raw).slice(Dice.dataoff, Dice.datasize), Base, Order);
  if (!Ranges)
    return Ranges.takeError();
  for (auto &R : *Ranges) {
    const auto Length = R.End - R.Begin;
    if (Packed) {
      R.Begin = Img.fileOffsetToVA(R.Begin);
      if (Length > InvalidVA - R.Begin)
        return llvm::createStringError(llvm::errc::invalid_argument, kRange);
      R.End = R.Begin + Length;
    }
    const auto *Seg = Img.getSegmentFor(R.Begin);
    const auto *Sec = Img.getSectionFor(R.Begin);
    if (!Seg || !Seg->isExecutable() ||
        !rangeInBounds(R.Begin - Seg->VA, Length, Seg->FileSz) ||
        (Sec && (!(Sec->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                                llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) ||
                 !rangeInBounds(R.Begin - Sec->VA, Length, Sec->FileSz))) ||
        (!Sec && llvm::any_of(Img.Sections, [&](const Section &S) {
          return S.Size && S.VA < R.End &&
                 (S.VA >= R.Begin || R.Begin - S.VA < S.Size);
        })))
      return llvm::createStringError(llvm::errc::invalid_argument, kOwner);
  }
  if (Packed)
    llvm::sort(*Ranges,
               [](const auto &L, const auto &R) { return L.Begin < R.Begin; });
  Img.MachODataInCode = std::move(*Ranges);
  return llvm::Error::success();
}
} // namespace neverd
