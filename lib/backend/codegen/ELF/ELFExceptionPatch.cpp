//===- ELFExceptionPatch.cpp - ELF unwind-record rewrite ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-elf-patch"

namespace neverd {

using namespace dweh;

namespace {

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "elf exception patch: " + Message);
}

/// Return the byte offset at which another `.eh_frame` sequence can be
/// appended. A zero-length record is a terminator, so it is replaced rather
/// than left between the original and regenerated records.  This is the same
/// record framing `.eh_frame` and `__eh_frame` share: a 4-byte length, or the
/// sentinel 0xffffffff followed by an 8-byte length for the 64-bit form.
std::optional<uint64_t> getEHFrameAppendOffset(llvm::ArrayRef<uint8_t> Bytes) {
  uint64_t Off = 0;
  while (Off < Bytes.size()) {
    if (!rangeInBounds(Off, sizeof(uint32_t), Bytes.size()))
      return std::nullopt;
    uint32_t Length = readLE<uint32_t>(Bytes.data() + Off);
    if (Length == 0) {
      for (uint8_t Byte : Bytes.drop_front(Off))
        if (Byte != 0)
          return std::nullopt;
      return Off;
    }

    uint64_t RecordSize = 0;
    if (Length == std::numeric_limits<uint32_t>::max()) {
      if (!rangeInBounds(Off, sizeof(uint32_t) + sizeof(uint64_t),
                         Bytes.size()))
        return std::nullopt;
      uint64_t ExtendedLength =
          readLE<uint64_t>(Bytes.data() + Off + sizeof(uint32_t));
      if (ExtendedLength > std::numeric_limits<uint64_t>::max() -
                               (sizeof(uint32_t) + sizeof(uint64_t)))
        return std::nullopt;
      RecordSize = sizeof(uint32_t) + sizeof(uint64_t) + ExtendedLength;
    } else {
      RecordSize = sizeof(uint32_t) + static_cast<uint64_t>(Length);
    }
    if (RecordSize == 0 || !rangeInBounds(Off, RecordSize, Bytes.size()))
      return std::nullopt;
    Off += RecordSize;
  }
  return Off;
}

/// A function the regenerated records describe, named the two ways the search
/// table needs it: the address that is unwound (the key it is sorted by) and
/// the address of the record that describes it.
struct FdeEntry {
  uint64_t InitLocVA = 0;
  uint64_t FdeVA = 0;
};

/// Encode a value little-endian at the width its DWARF EH format demands.
bool writeEncodedCount(std::vector<uint8_t> &Out, uint64_t Value, uint8_t Enc) {
  switch (getEncodedSize(Enc)) {
  case 2:
    if (Value > std::numeric_limits<uint16_t>::max())
      return false;
    Out.push_back(static_cast<uint8_t>(Value));
    Out.push_back(static_cast<uint8_t>(Value >> 8));
    return true;
  case 4:
    if (Value > std::numeric_limits<uint32_t>::max())
      return false;
    for (unsigned I = 0; I < 4; ++I)
      Out.push_back(static_cast<uint8_t>(Value >> (8 * I)));
    return true;
  case 8:
    for (unsigned I = 0; I < 8; ++I)
      Out.push_back(static_cast<uint8_t>(Value >> (8 * I)));
    return true;
  default:
    return false;
  }
}

bool pushSData4(std::vector<uint8_t> &Out, int64_t Value) {
  if (Value < std::numeric_limits<int32_t>::min() ||
      Value > std::numeric_limits<int32_t>::max())
    return false;
  auto U = static_cast<uint32_t>(static_cast<int32_t>(Value));
  for (unsigned I = 0; I < 4; ++I)
    Out.push_back(static_cast<uint8_t>(U >> (8 * I)));
  return true;
}

bool addSignedDelta(uint64_t Base, int64_t Delta, uint64_t &Result) {
  if (Delta >= 0) {
    const uint64_t Positive = static_cast<uint64_t>(Delta);
    if (Positive > std::numeric_limits<uint64_t>::max() - Base)
      return false;
    Result = Base + Positive;
    return true;
  }
  const uint64_t Magnitude = static_cast<uint64_t>(-(Delta + 1)) + 1;
  if (Magnitude > Base)
    return false;
  Result = Base - Magnitude;
  return true;
}

bool signedDelta(uint64_t Value, uint64_t Base, int64_t &Result) {
  if (Value >= Base) {
    const uint64_t Delta = Value - Base;
    if (Delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
    Result = static_cast<int64_t>(Delta);
    return true;
  }

  const uint64_t Magnitude = Base - Value;
  constexpr uint64_t MinMagnitude = uint64_t{1} << 63;
  if (Magnitude > MinMagnitude)
    return false;
  Result = Magnitude == MinMagnitude ? std::numeric_limits<int64_t>::min()
                                     : -static_cast<int64_t>(Magnitude);
  return true;
}

bool readHeaderPointer(const uint8_t *Bytes, size_t Size, size_t &Cursor,
                       uint8_t Encoding, uint64_t HeaderVA, bool Is64BitAddress,
                       uint64_t &Result) {
  if (Encoding == Omit || (Encoding & Indirect) != 0)
    return false;
  const uint8_t Format = getFormat(Encoding);
  const size_t Width =
      Format == Absptr ? (Is64BitAddress ? 8 : 4) : getEncodedSize(Encoding);
  if (Width == 0 || !rangeInBounds(Cursor, Width, Size))
    return false;

  uint64_t Unsigned = 0;
  int64_t Signed = 0;
  bool IsSigned = false;
  switch (Format) {
  case Absptr:
  case Udata4:
    Unsigned = readLE<uint32_t>(Bytes + Cursor);
    if (Width == 8)
      Unsigned = readLE<uint64_t>(Bytes + Cursor);
    break;
  case Udata2:
    Unsigned = readLE<uint16_t>(Bytes + Cursor);
    break;
  case Udata8:
    Unsigned = readLE<uint64_t>(Bytes + Cursor);
    break;
  case Sdata2:
    IsSigned = true;
    Signed = static_cast<int16_t>(readLE<uint16_t>(Bytes + Cursor));
    break;
  case Sdata4:
    IsSigned = true;
    Signed = static_cast<int32_t>(readLE<uint32_t>(Bytes + Cursor));
    break;
  case Sdata8:
    IsSigned = true;
    Signed = static_cast<int64_t>(readLE<uint64_t>(Bytes + Cursor));
    break;
  default:
    return false;
  }

  uint64_t Base = 0;
  switch (getApplication(Encoding)) {
  case AbsoluteApp:
    break;
  case PCRel:
    if (Cursor > std::numeric_limits<uint64_t>::max() - HeaderVA)
      return false;
    Base = HeaderVA + Cursor;
    break;
  default:
    return false;
  }
  if (IsSigned) {
    if (!addSignedDelta(Base, Signed, Result))
      return false;
  } else {
    if (Unsigned > std::numeric_limits<uint64_t>::max() - Base)
      return false;
    Result = Base + Unsigned;
  }
  Cursor += Width;
  return true;
}

/// Merge the existing `.eh_frame_hdr` search table with entries for the
/// appended functions and re-emit the whole section.  Only the encoding real
/// toolchains produce -- a `datarel sdata4` table over an `.eh_frame_hdr`
/// relative base -- is rewritten; any other shape returns false and the caller
/// fails closed.
bool rebuildEhFrameHdr(llvm::ArrayRef<uint8_t> Binary,
                       const ELFEHFrameRegion &Region,
                       const std::vector<FdeEntry> &NewEntries,
                       std::vector<uint8_t> &Out) {
  if (Region.HdrSize < kEhFrameHdrMinSize ||
      !rangeInBounds(Region.HdrFileOff, Region.HdrSize, Binary.size()))
    return false;

  if (Region.AppendFileOff < Region.SectionFileOff ||
      Region.AppendVA < Region.SectionVA ||
      Region.AppendFileOff - Region.SectionFileOff !=
          Region.AppendVA - Region.SectionVA)
    return false;
  const uint64_t ExistingSize = Region.AppendFileOff - Region.SectionFileOff;
  if (!rangeInBounds(Region.SectionFileOff, ExistingSize, Binary.size()))
    return false;
  auto ExistingRecords = decodeDwarfEHFrameRecords(
      llvm::ArrayRef<uint8_t>(Binary.data() + Region.SectionFileOff,
                              static_cast<size_t>(ExistingSize)),
      Region.SectionVA, Region.Is64);
  if (!ExistingRecords) {
    llvm::consumeError(ExistingRecords.takeError());
    return false;
  }
  std::map<uint64_t, uint64_t> ExistingByRecord;
  for (const DwarfEHFrameRecord &Record : *ExistingRecords)
    if (!ExistingByRecord.emplace(Record.RecordVA, Record.BeginVA).second)
      return false;

  const uint8_t *H = Binary.data() + Region.HdrFileOff;
  const size_t HN = static_cast<size_t>(Region.HdrSize);
  auto Hdr = *reinterpret_cast<const EhFrameHdrHeader *>(H);
  if (Hdr.Version != kEhFrameHdrVersion)
    return false;
  if ((Hdr.TableEnc & Indirect) != 0 ||
      getApplication(Hdr.TableEnc) != DataRel ||
      getFormat(Hdr.TableEnc) != Sdata4)
    return false;
  if ((Hdr.FdeCountEnc & Indirect) != 0 ||
      getApplication(Hdr.FdeCountEnc) != AbsoluteApp ||
      (getFormat(Hdr.FdeCountEnc) != Udata2 &&
       getFormat(Hdr.FdeCountEnc) != Udata4 &&
       getFormat(Hdr.FdeCountEnc) != Udata8))
    return false;

  size_t Cursor = sizeof(EhFrameHdrHeader);
  uint64_t EHFramePointer = 0;
  if (!readHeaderPointer(H, HN, Cursor, Hdr.EhFramePtrEnc, Region.HdrVA,
                         Region.Is64, EHFramePointer) ||
      EHFramePointer != Region.SectionVA)
    return false;
  const size_t PrefixEnd = Cursor; // header + eh_frame_ptr, copied verbatim

  size_t CountSize = getEncodedSize(Hdr.FdeCountEnc);
  if (CountSize == 0 || !rangeInBounds(Cursor, CountSize, HN))
    return false;
  uint64_t OldCount =
      static_cast<uint64_t>(readEncoded(H, HN, Cursor, Hdr.FdeCountEnc));

  if (OldCount > (HN - Cursor) / kFdeEntrySize ||
      OldCount != ExistingByRecord.size())
    return false;

  std::map<uint64_t, uint64_t> Entries;
  std::set<uint64_t> SeenOldRecords;
  uint64_t PreviousOld = 0;
  bool HasPreviousOld = false;
  for (uint64_t I = 0; I < OldCount; ++I) {
    int64_t InitRel = readEncoded(H, HN, Cursor, Hdr.TableEnc);
    int64_t FdeRel = readEncoded(H, HN, Cursor, Hdr.TableEnc);
    uint64_t InitVA = 0;
    uint64_t FdeVA = 0;
    if (!addSignedDelta(Region.HdrVA, InitRel, InitVA) ||
        !addSignedDelta(Region.HdrVA, FdeRel, FdeVA) ||
        (HasPreviousOld && InitVA <= PreviousOld) ||
        !Entries.emplace(InitVA, FdeVA).second)
      return false;
    auto Existing = ExistingByRecord.find(FdeVA);
    if (Existing == ExistingByRecord.end() || Existing->second != InitVA ||
        !SeenOldRecords.insert(FdeVA).second)
      return false;
    PreviousOld = InitVA;
    HasPreviousOld = true;
  }
  if (SeenOldRecords.size() != ExistingByRecord.size())
    return false;

  std::set<uint64_t> NewKeys;
  for (const FdeEntry &Entry : NewEntries) {
    if (!NewKeys.insert(Entry.InitLocVA).second)
      return false;
    // A regenerated FDE supersedes the old record for the same function.
    Entries[Entry.InitLocVA] = Entry.FdeVA;
  }

  Out.assign(H, H + PrefixEnd);
  if (!writeEncodedCount(Out, Entries.size(), Hdr.FdeCountEnc))
    return false;
  uint64_t Previous = 0;
  bool HasPrevious = false;
  for (const auto &[InitVA, FdeVA] : Entries) {
    if ((HasPrevious && InitVA <= Previous))
      return false;
    int64_t InitRel = 0;
    int64_t FdeRel = 0;
    if (!signedDelta(InitVA, Region.HdrVA, InitRel) ||
        !signedDelta(FdeVA, Region.HdrVA, FdeRel) ||
        !pushSData4(Out, InitRel) || !pushSData4(Out, FdeRel))
      return false;
    Previous = InitVA;
    HasPrevious = true;
  }
  return true;
}

void growELFSection(std::vector<uint8_t> &Binary, uint64_t HeaderOff, bool Is64,
                    uint64_t NewSize) {
  if (!rangeInBounds(HeaderOff, getELFShdrSize(Is64), Binary.size()))
    return;
  ELFShdrFields F = readELFShdr(Binary.data() + HeaderOff, Is64);
  F.Size = NewSize;
  writeELFShdr(Binary.data() + HeaderOff, Is64, F);
}

/// Install the regenerated records and the search-table entries that make them
/// findable, or leave the image untouched and report failure.  Nothing is
/// written until every part is known to fit, so a rejected install never leaves
/// a half-rewritten table behind.
bool installRecordsAndTable(std::vector<uint8_t> &Binary,
                            const ELFEHFrameRegion &Region,
                            const CompiledSection &Generated,
                            llvm::ArrayRef<uint64_t> RequiredFunctions) {
  const std::optional<ELFEHFrameRegion> Current = findELFEHFrameRegion(Binary);
  if (!Current || Current->Is64 != Region.Is64 ||
      Current->SectionVA != Region.SectionVA ||
      Current->SectionFileOff != Region.SectionFileOff ||
      Current->AppendVA != Region.AppendVA ||
      Current->AppendFileOff != Region.AppendFileOff ||
      Current->LimitFileOff != Region.LimitFileOff ||
      Current->SectionHeaderOff != Region.SectionHeaderOff ||
      Current->HasHdr != Region.HasHdr || Current->HdrVA != Region.HdrVA ||
      Current->HdrFileOff != Region.HdrFileOff ||
      Current->HdrSize != Region.HdrSize ||
      Current->HdrLimitFileOff != Region.HdrLimitFileOff ||
      Current->HdrSectionHeaderOff != Region.HdrSectionHeaderOff ||
      Current->GnuEhFramePhdrOff != Region.GnuEhFramePhdrOff)
    return false;

  if (!Region.HasHdr || Generated.VA != Region.AppendVA ||
      Generated.Size != Generated.ExternalBytes.size() ||
      Region.LimitFileOff < Region.AppendFileOff ||
      Region.HdrLimitFileOff < Region.HdrFileOff ||
      Region.AppendVA < Region.SectionVA ||
      Region.AppendFileOff < Region.SectionFileOff ||
      Region.AppendVA - Region.SectionVA !=
          Region.AppendFileOff - Region.SectionFileOff ||
      Generated.Size > Region.LimitFileOff - Region.AppendFileOff ||
      !rangeInBounds(Region.AppendFileOff, Generated.Size, Binary.size()) ||
      !rangeInBounds(Region.SectionHeaderOff, getELFShdrSize(Region.Is64),
                     Binary.size()) ||
      !rangeInBounds(Region.HdrSectionHeaderOff, getELFShdrSize(Region.Is64),
                     Binary.size()) ||
      !rangeInBounds(Region.GnuEhFramePhdrOff, getELFPhdrSize(Region.Is64),
                     Binary.size()))
    return false;

  const uint64_t ExistingEHSize = Region.AppendVA - Region.SectionVA;
  if (Generated.ExternalBytes.size() >
      std::numeric_limits<uint64_t>::max() - ExistingEHSize)
    return false;
  const uint64_t EhFrameSize = ExistingEHSize + Generated.ExternalBytes.size();

  const ELFShdrFields EHSection =
      readELFShdr(Binary.data() + Region.SectionHeaderOff, Region.Is64);
  const ELFShdrFields HdrSection =
      readELFShdr(Binary.data() + Region.HdrSectionHeaderOff, Region.Is64);
  const ELFPhdrFields GnuHeader =
      readELFPhdr(Binary.data() + Region.GnuEhFramePhdrOff, Region.Is64);
  if (EHSection.Type != llvm::ELF::SHT_PROGBITS ||
      (EHSection.Flags & llvm::ELF::SHF_ALLOC) == 0 ||
      EHSection.Offset != Region.SectionFileOff ||
      EHSection.Addr != Region.SectionVA ||
      !rangeInBounds(EHSection.Offset, EHSection.Size, Binary.size()) ||
      HdrSection.Type != llvm::ELF::SHT_PROGBITS ||
      (HdrSection.Flags & llvm::ELF::SHF_ALLOC) == 0 ||
      HdrSection.Offset != Region.HdrFileOff ||
      HdrSection.Addr != Region.HdrVA || HdrSection.Size != Region.HdrSize ||
      GnuHeader.Type != llvm::ELF::PT_GNU_EH_FRAME ||
      GnuHeader.Offset != Region.HdrFileOff ||
      GnuHeader.VAddr != Region.HdrVA || GnuHeader.FileSz != Region.HdrSize ||
      GnuHeader.MemSz != Region.HdrSize)
    return false;
  const auto LogicalAppend = getEHFrameAppendOffset(llvm::ArrayRef<uint8_t>(
      Binary.data() + EHSection.Offset, static_cast<size_t>(EHSection.Size)));
  if (!LogicalAppend || *LogicalAppend != ExistingEHSize)
    return false;

  auto Decoded = decodeDwarfEHFrameRecords(Generated.ExternalBytes,
                                           Region.AppendVA, Region.Is64);
  if (!Decoded) {
    llvm::consumeError(Decoded.takeError());
    return false;
  }
  std::vector<FdeEntry> NewEntries;
  NewEntries.reserve(Decoded->size());
  for (const DwarfEHFrameRecord &Record : *Decoded)
    NewEntries.push_back({Record.BeginVA, Record.RecordVA});
  for (uint64_t Address : RequiredFunctions)
    if (std::none_of(Decoded->begin(), Decoded->end(),
                     [&](const DwarfEHFrameRecord &Record) {
                       return Record.BeginVA == Address &&
                              Record.covers(Address);
                     }))
      return false;

  std::vector<uint8_t> NewHdr;
  if (!rebuildEhFrameHdr(Binary, Region, NewEntries, NewHdr))
    return false;
  if (NewHdr.size() > Region.HdrLimitFileOff - Region.HdrFileOff ||
      !rangeInBounds(Region.HdrFileOff, NewHdr.size(), Binary.size()) ||
      (!Region.Is64 && (EhFrameSize > std::numeric_limits<uint32_t>::max() ||
                        NewHdr.size() > std::numeric_limits<uint32_t>::max())))
    return false;

  // Everything fits; commit.
  if (!Generated.ExternalBytes.empty())
    std::memcpy(Binary.data() + Region.AppendFileOff,
                Generated.ExternalBytes.data(), Generated.ExternalBytes.size());
  growELFSection(Binary, Region.SectionHeaderOff, Region.Is64, EhFrameSize);

  std::memcpy(Binary.data() + Region.HdrFileOff, NewHdr.data(), NewHdr.size());
  growELFSection(Binary, Region.HdrSectionHeaderOff, Region.Is64,
                 NewHdr.size());

  ELFPhdrFields PH =
      readELFPhdr(Binary.data() + Region.GnuEhFramePhdrOff, Region.Is64);
  PH.FileSz = NewHdr.size();
  PH.MemSz = NewHdr.size();
  writeELFPhdr(Binary.data() + Region.GnuEhFramePhdrOff, Region.Is64, PH);
  return true;
}

} // namespace

std::optional<ELFEHFrameRegion>
findELFEHFrameRegion(llvm::ArrayRef<uint8_t> Binary) {
  const uint8_t *Data = Binary.data();
  const size_t Size = Binary.size();
  ELFHeaderInfo Hdr;
  if (!validateELFHeaderTables(Data, Size, Hdr))
    return std::nullopt;

  uint64_t ShStrOff =
      Hdr.ShOff + static_cast<uint64_t>(Hdr.ShStrNdx) * Hdr.ShEntSize;
  if (!rangeInBounds(ShStrOff, Hdr.ShEntSize, Size))
    return std::nullopt;
  auto ShStr = readELFShdr(Data + ShStrOff, Hdr.Is64);
  if (ShStr.Type != llvm::ELF::SHT_STRTAB ||
      !rangeInBounds(ShStr.Offset, ShStr.Size, Size))
    return std::nullopt;

  struct Section {
    ELFShdrFields F;
    uint64_t HeaderOff = 0;
  };
  std::optional<Section> EhFrame, EhFrameHdr;
  unsigned EhFrameCount = 0;
  unsigned EhFrameHdrCount = 0;
  bool InvalidSectionName = false;
  std::vector<uint64_t> SectionOffsets;
  forEachELFShdr(Data, Size, [&](const ELFShdrFields &F, uint16_t I) {
    uint64_t HeaderOff = Hdr.ShOff + static_cast<uint64_t>(I) * Hdr.ShEntSize;
    if (F.Type != llvm::ELF::SHT_NOBITS && F.Offset != 0)
      SectionOffsets.push_back(F.Offset);
    auto Name = readELFSectionName(Data, Size, ShStr, F.Name);
    if (!Name) {
      InvalidSectionName = true;
      return;
    }
    if (*Name == section_names::elf::EhFrame) {
      ++EhFrameCount;
      if (!EhFrame)
        EhFrame = Section{F, HeaderOff};
    } else if (*Name == section_names::elf::EhFrameHdr) {
      ++EhFrameHdrCount;
      if (!EhFrameHdr)
        EhFrameHdr = Section{F, HeaderOff};
    }
  });
  if (InvalidSectionName || !EhFrame || EhFrameCount != 1 ||
      EhFrameHdrCount > 1)
    return std::nullopt;

  const ELFShdrFields &EF = EhFrame->F;
  if (EF.Type != llvm::ELF::SHT_PROGBITS || EF.Size == 0 || EF.Offset == 0 ||
      (EF.Flags & llvm::ELF::SHF_ALLOC) == 0 ||
      !rangeInBounds(EF.Offset, EF.Size, Size))
    return std::nullopt;

  auto Logical = getEHFrameAppendOffset(
      llvm::ArrayRef<uint8_t>(Data + EF.Offset, static_cast<size_t>(EF.Size)));
  if (!Logical || *Logical > EF.Size ||
      EF.Addr > std::numeric_limits<uint64_t>::max() - *Logical ||
      EF.Offset > std::numeric_limits<uint64_t>::max() - *Logical)
    return std::nullopt;

  uint64_t AppendFileOff = EF.Offset + *Logical;

  // The append has to stay inside the loadable segment that maps `.eh_frame`,
  // and before whatever section follows it there.
  uint64_t Limit = Size;
  unsigned SegmentCount = 0;
  forEachELFPhdr(Data, Size,
                 [&](const ELFPhdrFields &P, const uint8_t *, bool) {
                   if (P.Type != llvm::ELF::PT_LOAD)
                     return;
                   if (!rangeInBounds(P.Offset, P.FileSz, Size) ||
                       P.MemSz < P.FileSz || P.Offset > EF.Offset)
                     return;
                   const uint64_t Delta = EF.Offset - P.Offset;
                   if (Delta > P.FileSz || EF.Size > P.FileSz - Delta ||
                       Delta > std::numeric_limits<uint64_t>::max() - P.VAddr ||
                       P.VAddr + Delta != EF.Addr)
                     return;
                   Limit = std::min(Limit, P.Offset + P.FileSz);
                   ++SegmentCount;
                 });
  if (SegmentCount != 1)
    return std::nullopt;
  for (uint64_t Offset : SectionOffsets)
    if (Offset > AppendFileOff)
      Limit = std::min(Limit, Offset);
  if (Limit < AppendFileOff || Limit > Size)
    return std::nullopt;

  ELFEHFrameRegion Region;
  Region.Is64 = Hdr.Is64;
  Region.SectionVA = EF.Addr;
  Region.SectionFileOff = EF.Offset;
  Region.AppendVA = EF.Addr + *Logical;
  Region.AppendFileOff = AppendFileOff;
  Region.LimitFileOff = Limit;
  Region.SectionHeaderOff = EhFrame->HeaderOff;

  if (EhFrameHdr) {
    const ELFShdrFields &HF = EhFrameHdr->F;
    if (HF.Type == llvm::ELF::SHT_PROGBITS && HF.Offset != 0 &&
        HF.Size >= kEhFrameHdrMinSize &&
        (HF.Flags & llvm::ELF::SHF_ALLOC) != 0 &&
        rangeInBounds(HF.Offset, HF.Size, Size)) {
      uint64_t HdrLimit = Size;
      unsigned HdrSegmentCount = 0;
      forEachELFPhdr(
          Data, Size, [&](const ELFPhdrFields &P, const uint8_t *, bool) {
            if (P.Type != llvm::ELF::PT_LOAD)
              return;
            if (!rangeInBounds(P.Offset, P.FileSz, Size) ||
                P.MemSz < P.FileSz || P.Offset > HF.Offset)
              return;
            const uint64_t Delta = HF.Offset - P.Offset;
            if (Delta > P.FileSz || HF.Size > P.FileSz - Delta ||
                Delta > std::numeric_limits<uint64_t>::max() - P.VAddr ||
                P.VAddr + Delta != HF.Addr)
              return;
            HdrLimit = std::min(HdrLimit, P.Offset + P.FileSz);
            ++HdrSegmentCount;
          });
      for (uint64_t Offset : SectionOffsets)
        if (Offset > HF.Offset)
          HdrLimit = std::min(HdrLimit, Offset);

      uint64_t GnuOff = 0;
      unsigned GnuHeaderCount = 0;
      unsigned GnuCount = 0;
      forEachELFPhdr(Data, Size,
                     [&](const ELFPhdrFields &P, const uint8_t *Ptr, bool) {
                       if (P.Type != llvm::ELF::PT_GNU_EH_FRAME)
                         return;
                       ++GnuHeaderCount;
                       if (P.Offset != HF.Offset || P.VAddr != HF.Addr ||
                           P.FileSz != HF.Size || P.MemSz != HF.Size)
                         return;
                       ++GnuCount;
                       GnuOff = static_cast<uint64_t>(Ptr - Data);
                     });

      if (HdrSegmentCount == 1 && GnuHeaderCount == 1 && GnuCount == 1 &&
          GnuOff != 0 && HdrLimit >= HF.Offset + HF.Size) {
        Region.HasHdr = true;
        Region.HdrVA = HF.Addr;
        Region.HdrFileOff = HF.Offset;
        Region.HdrSize = HF.Size;
        Region.HdrLimitFileOff = HdrLimit;
        Region.HdrSectionHeaderOff = EhFrameHdr->HeaderOff;
        Region.GnuEhFramePhdrOff = GnuOff;
      }
    }
  }
  return Region;
}

bool requiresRegisteredELFEHFrame(const llvm::Module &Mod) {
  auto Requirements = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Requirements) {
    llvm::consumeError(Requirements.takeError());
    return true;
  }
  return Requirements->RequiresRegisteredUnwind;
}

llvm::Error installELFEHFrame(std::vector<uint8_t> &Binary,
                              const std::optional<ELFEHFrameRegion> &Region,
                              const CompiledImage &Compiled,
                              const llvm::Module &Mod) {
  auto Requirements = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Requirements)
    return Requirements.takeError();
  const bool Required = Requirements->RequiresRegisteredUnwind;
  if (Required && !Compiled.Unresolved.empty())
    return patchError("required unwind output has unresolved symbols");

  auto RequiredFunctions = exception_rewrite::resolveRequiredFunctionAddresses(
      *Requirements, Compiled);
  if (!RequiredFunctions)
    return RequiredFunctions.takeError();

  const CompiledSection *Generated = nullptr;
  for (const CompiledSection &Section : Compiled.Sections)
    if (Section.IsAllocated && Section.Name == section_names::elf::EhFrame) {
      if (Generated)
        return patchError("multiple regenerated .eh_frame sections");
      Generated = &Section;
    }

  // A generated section left inside the patch image lives in the appended
  // segment, which the original `PT_GNU_EH_FRAME` does not cover, so it cannot
  // register.  With no exception contract that is fine; with one it is fatal.
  if (!Generated || Generated->IsInImage) {
    if (Required)
      return patchError("no registrable .eh_frame produced");
    return llvm::Error::success();
  }

  const bool Registered =
      Region &&
      installRecordsAndTable(Binary, *Region, *Generated, *RequiredFunctions);
  if (!Registered && Required)
    return patchError("cannot register regenerated .eh_frame");

  LLVM_DEBUG({
    if (!Registered)
      llvm::dbgs() << "elf exception patch: omitting unregistered CFI-only "
                      ".eh_frame records\n";
  });
  return llvm::Error::success();
}

} // namespace neverd
