//===- ELFExceptionPatch.cpp - ELF unwind-record rewrite ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"

#include "neverd/Object/ELFLayout.h"
#include "neverd/Object/SectionNames.h"
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/DwarfEH.h"
#include "neverd/backend/codegen/BinaryRewriter.h"

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
#include <vector>

#define DEBUG_TYPE "neverd-elf-patch"

namespace neverd {

using namespace dweh;

namespace {

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "elf exception patch: " + Message);
}

/// Return the byte offset at which another `.eh_frame` sequence can be appended.
/// A zero-length record is a terminator, so it is replaced rather than left
/// between the original and regenerated records.  This is the same record
/// framing `.eh_frame` and `__eh_frame` share: a 4-byte length, or the sentinel
/// 0xffffffff followed by an 8-byte length for the 64-bit form.
std::optional<uint64_t> getEHFrameAppendOffset(llvm::ArrayRef<uint8_t> Bytes) {
  uint64_t Off = 0;
  while (Off < Bytes.size()) {
    if (!rangeInBounds(Off, sizeof(uint32_t), Bytes.size()))
      return std::nullopt;
    uint32_t Length = readLE<uint32_t>(Bytes.data() + Off);
    if (Length == 0)
      return Off;

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

bool readULEBBounded(const uint8_t *B, size_t N, size_t &C, uint64_t &Out) {
  Out = 0;
  for (unsigned Shift = 0; Shift < 64; Shift += 7) {
    if (C >= N)
      return false;
    uint8_t Byte = B[C++];
    Out |= static_cast<uint64_t>(Byte & 0x7f) << Shift;
    if (!(Byte & 0x80))
      return true;
  }
  return false;
}

bool skipSLEBBounded(const uint8_t *B, size_t N, size_t &C) {
  for (unsigned I = 0; I < 10; ++I) {
    if (C >= N)
      return false;
    if (!(B[C++] & 0x80))
      return true;
  }
  return false;
}

/// Read the FDE pointer encoding a CIE declares in its `R` augmentation.  A CIE
/// with no `z` augmentation, or none carrying an `R`, leaves the encoding at the
/// psABI default of an absolute pointer.  Returns false only when the bytes run
/// out, which fails the whole install closed rather than guessing.
bool cieFdeEncoding(const uint8_t *B, size_t N, size_t AfterId, uint8_t &Enc) {
  Enc = Absptr;
  size_t C = AfterId;
  if (C >= N)
    return false;
  uint8_t Version = B[C++];
  if (Version != 1 && Version != 3 && Version != 4)
    return false;

  size_t AugStart = C;
  while (C < N && B[C] != 0)
    ++C;
  if (C >= N)
    return false;
  const char *Aug = reinterpret_cast<const char *>(B + AugStart);
  size_t AugLen = C - AugStart;
  ++C; // the NUL

  if (Version == 4) {
    // address_size, segment_selector_size.
    if (C + 2 > N)
      return false;
    C += 2;
  }

  uint64_t Scratch;
  if (!readULEBBounded(B, N, C, Scratch)) // code_alignment_factor
    return false;
  if (!skipSLEBBounded(B, N, C)) // data_alignment_factor
    return false;
  if (!readULEBBounded(B, N, C, Scratch)) // return_address_register
    return false;

  if (AugLen == 0 || Aug[0] != 'z')
    return true; // no augmentation data; default encoding stands

  uint64_t AugDataLen;
  if (!readULEBBounded(B, N, C, AugDataLen))
    return false;
  size_t AugDataEnd = C + static_cast<size_t>(AugDataLen);
  if (AugDataLen > N || AugDataEnd > N)
    return false;

  for (size_t I = 1; I < AugLen; ++I) {
    switch (Aug[I]) {
    case 'L':
      if (C + 1 > AugDataEnd)
        return false;
      C += 1;
      break;
    case 'P': {
      if (C + 1 > AugDataEnd)
        return false;
      uint8_t PEnc = B[C++];
      size_t PSize = getEncodedSize(PEnc);
      if (PSize == 0 || C + PSize > AugDataEnd)
        return false;
      C += PSize;
      break;
    }
    case 'R':
      if (C + 1 > AugDataEnd)
        return false;
      Enc = B[C++];
      return true;
    case 'S':
    case 'B':
    case 'G':
      break;
    default:
      return false;
    }
  }
  return true;
}

/// Resolve an FDE `initial_location` field to the runtime address it names.
/// Only the applications an FDE actually uses are accepted; anything else fails
/// closed so a misread never plants a wrong key in the search table.
bool resolveInitLoc(const uint8_t *B, size_t N, size_t Cursor, uint64_t FieldVA,
                    uint8_t Enc, bool Is64, uint64_t &Out) {
  int64_t Raw = 0;
  if (getFormat(Enc) == Absptr) {
    size_t Size = Is64 ? 8 : 4;
    if (Cursor + Size > N)
      return false;
    if (Size == 8)
      Raw = static_cast<int64_t>(readLE<uint64_t>(B + Cursor));
    else
      Raw = static_cast<int32_t>(readLE<uint32_t>(B + Cursor));
  } else {
    size_t Size = getEncodedSize(Enc);
    if (Size == 0 || Cursor + Size > N)
      return false;
    Raw = readEncoded(B, N, Cursor, Enc);
  }

  switch (getApplication(Enc)) {
  case AbsoluteApp:
    Out = static_cast<uint64_t>(Raw);
    return true;
  case PCRel:
    Out = FieldVA + static_cast<uint64_t>(Raw);
    return true;
  default:
    return false;
  }
}

/// Walk the regenerated `.eh_frame` fragment mapped at \p BaseVA and collect one
/// entry per function it describes.  Returns false on any malformed record so a
/// partially understood table is never installed.
bool collectFDEs(llvm::ArrayRef<uint8_t> Bytes, uint64_t BaseVA, bool Is64,
                 std::vector<FdeEntry> &Out) {
  const uint8_t *B = Bytes.data();
  const size_t N = Bytes.size();
  std::map<uint64_t, uint8_t> CIEEncByStart;

  uint64_t Off = 0;
  while (Off < N) {
    if (!rangeInBounds(Off, sizeof(uint32_t), N))
      return false;
    uint32_t Length = readLE<uint32_t>(B + Off);
    if (Length == 0)
      break;

    bool Extended = Length == std::numeric_limits<uint32_t>::max();
    uint64_t FieldLen = Extended ? (sizeof(uint32_t) + sizeof(uint64_t))
                                 : sizeof(uint32_t);
    uint64_t Payload =
        Extended ? readLE<uint64_t>(B + Off + sizeof(uint32_t)) : Length;
    if (Payload > N || FieldLen > N - Off || Payload > N - Off - FieldLen)
      return false;
    uint64_t RecordEnd = Off + FieldLen + Payload;

    uint64_t IdOff = Off + FieldLen;
    uint64_t IdSize = Extended ? sizeof(uint64_t) : sizeof(uint32_t);
    if (IdSize > RecordEnd - IdOff)
      return false;
    uint64_t Id = Extended ? readLE<uint64_t>(B + IdOff)
                           : readLE<uint32_t>(B + IdOff);

    if (Id == 0) {
      uint8_t Enc = Absptr;
      if (!cieFdeEncoding(B, static_cast<size_t>(RecordEnd),
                          static_cast<size_t>(IdOff + IdSize), Enc))
        return false;
      CIEEncByStart[Off] = Enc;
    } else {
      if (Id > IdOff)
        return false;
      uint64_t CIEStart = IdOff - Id;
      auto It = CIEEncByStart.find(CIEStart);
      if (It == CIEEncByStart.end())
        return false;

      uint64_t InitOff = IdOff + IdSize;
      uint64_t InitLoc = 0;
      if (!resolveInitLoc(B, static_cast<size_t>(RecordEnd),
                          static_cast<size_t>(InitOff), BaseVA + InitOff,
                          It->second, Is64, InitLoc))
        return false;
      Out.push_back({InitLoc, BaseVA + Off});
    }

    if (RecordEnd <= Off)
      return false;
    Off = RecordEnd;
  }
  return true;
}

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

  const uint8_t *H = Binary.data() + Region.HdrFileOff;
  const size_t HN = static_cast<size_t>(Region.HdrSize);
  auto Hdr = *reinterpret_cast<const EhFrameHdrHeader *>(H);
  if (Hdr.Version != kEhFrameHdrVersion)
    return false;
  if (getApplication(Hdr.TableEnc) != DataRel ||
      getFormat(Hdr.TableEnc) != Sdata4)
    return false;

  size_t Cursor = sizeof(EhFrameHdrHeader);
  size_t PtrSize = getEncodedSize(Hdr.EhFramePtrEnc);
  if (PtrSize == 0 || Cursor + PtrSize > HN)
    return false;
  size_t PrefixEnd = Cursor + PtrSize; // header + eh_frame_ptr, copied verbatim
  Cursor = PrefixEnd;

  size_t CountSize = getEncodedSize(Hdr.FdeCountEnc);
  if (CountSize == 0 || !rangeInBounds(Cursor, CountSize, HN))
    return false;
  uint64_t OldCount = static_cast<uint64_t>(
      readEncoded(H, HN, Cursor, Hdr.FdeCountEnc));

  if (OldCount > (HN - Cursor) / kFdeEntrySize)
    return false;

  std::vector<FdeEntry> Entries;
  Entries.reserve(static_cast<size_t>(OldCount) + NewEntries.size());
  for (uint64_t I = 0; I < OldCount; ++I) {
    int64_t InitRel = readEncoded(H, HN, Cursor, Hdr.TableEnc);
    int64_t FdeRel = readEncoded(H, HN, Cursor, Hdr.TableEnc);
    Entries.push_back({Region.HdrVA + static_cast<uint64_t>(InitRel),
                       Region.HdrVA + static_cast<uint64_t>(FdeRel)});
  }
  Entries.insert(Entries.end(), NewEntries.begin(), NewEntries.end());

  // The table is a binary search structure; it is only usable sorted by the
  // address each entry unwinds.
  std::stable_sort(Entries.begin(), Entries.end(),
                   [](const FdeEntry &A, const FdeEntry &B) {
                     return A.InitLocVA < B.InitLocVA;
                   });

  Out.assign(H, H + PrefixEnd);
  if (!writeEncodedCount(Out, Entries.size(), Hdr.FdeCountEnc))
    return false;
  for (const FdeEntry &E : Entries) {
    if (!pushSData4(Out, static_cast<int64_t>(E.InitLocVA) -
                             static_cast<int64_t>(Region.HdrVA)) ||
        !pushSData4(Out, static_cast<int64_t>(E.FdeVA) -
                             static_cast<int64_t>(Region.HdrVA)))
      return false;
  }
  return true;
}

void growELFSection(std::vector<uint8_t> &Binary, uint64_t HeaderOff,
                    bool Is64, uint64_t NewSize) {
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
                            const CompiledSection &Generated) {
  if (!Region.HasHdr || Generated.VA != Region.AppendVA ||
      Generated.Size != Generated.ExternalBytes.size() ||
      Generated.Size > Region.LimitFileOff - Region.AppendFileOff ||
      !rangeInBounds(Region.AppendFileOff, Generated.Size, Binary.size()))
    return false;

  std::vector<FdeEntry> NewEntries;
  if (!collectFDEs(Generated.ExternalBytes, Region.AppendVA, Region.Is64,
                   NewEntries))
    return false;

  std::vector<uint8_t> NewHdr;
  if (!rebuildEhFrameHdr(Binary, Region, NewEntries, NewHdr))
    return false;
  if (NewHdr.size() > Region.HdrLimitFileOff - Region.HdrFileOff ||
      !rangeInBounds(Region.HdrFileOff, NewHdr.size(), Binary.size()) ||
      !rangeInBounds(Region.GnuEhFramePhdrOff, getELFPhdrSize(Region.Is64),
                     Binary.size()))
    return false;

  // Everything fits; commit.
  if (!Generated.ExternalBytes.empty())
    std::memcpy(Binary.data() + Region.AppendFileOff,
                Generated.ExternalBytes.data(), Generated.ExternalBytes.size());
  uint64_t EhFrameSize = Region.AppendVA - Region.SectionVA +
                         Generated.ExternalBytes.size();
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
  auto Hdr = parseELFHeader(Data, Size);
  if (Hdr.HeaderSize == 0 || Hdr.ShNum == 0 || Hdr.ShStrNdx == 0 ||
      Hdr.ShEntSize == 0)
    return std::nullopt;

  uint64_t ShStrOff =
      Hdr.ShOff + static_cast<uint64_t>(Hdr.ShStrNdx) * Hdr.ShEntSize;
  if (!rangeInBounds(ShStrOff, Hdr.ShEntSize, Size))
    return std::nullopt;
  auto ShStr = readELFShdr(Data + ShStrOff, Hdr.Is64);
  if (!rangeInBounds(ShStr.Offset, ShStr.Size, Size))
    return std::nullopt;
  const char *StrTab = reinterpret_cast<const char *>(Data + ShStr.Offset);

  struct Section {
    ELFShdrFields F;
    uint64_t HeaderOff = 0;
  };
  std::optional<Section> EhFrame, EhFrameHdr;
  std::vector<uint64_t> SectionOffsets;
  forEachELFShdr(Data, Size, [&](const ELFShdrFields &F, uint16_t I) {
    uint64_t HeaderOff =
        Hdr.ShOff + static_cast<uint64_t>(I) * Hdr.ShEntSize;
    if (F.Type != llvm::ELF::SHT_NOBITS && F.Offset != 0)
      SectionOffsets.push_back(F.Offset);
    if (F.Name >= ShStr.Size)
      return;
    llvm::StringRef Name(StrTab + F.Name,
                         static_cast<size_t>(ShStr.Size - F.Name));
    Name = Name.split('\0').first;
    if (Name == section_names::elf::EhFrame && !EhFrame)
      EhFrame = Section{F, HeaderOff};
    else if (Name == section_names::elf::EhFrameHdr && !EhFrameHdr)
      EhFrameHdr = Section{F, HeaderOff};
  });
  if (!EhFrame)
    return std::nullopt;

  const ELFShdrFields &EF = EhFrame->F;
  if (EF.Size == 0 || EF.Offset == 0 ||
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
  bool InSegment = false;
  forEachELFPhdr(Data, Size,
                 [&](const ELFPhdrFields &P, const uint8_t *, bool) {
                   if (P.Type != llvm::ELF::PT_LOAD)
                     return;
                   if (P.Offset <= EF.Offset &&
                       EF.Offset < P.Offset + P.FileSz) {
                     Limit = std::min(Limit, P.Offset + P.FileSz);
                     InSegment = true;
                   }
                 });
  if (!InSegment)
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
    if (HF.Offset != 0 && HF.Size >= kEhFrameHdrMinSize &&
        rangeInBounds(HF.Offset, HF.Size, Size)) {
      uint64_t HdrLimit = Size;
      bool HdrInSegment = false;
      forEachELFPhdr(Data, Size,
                     [&](const ELFPhdrFields &P, const uint8_t *, bool) {
                       if (P.Type != llvm::ELF::PT_LOAD)
                         return;
                       if (P.Offset <= HF.Offset &&
                           HF.Offset < P.Offset + P.FileSz) {
                         HdrLimit = std::min(HdrLimit, P.Offset + P.FileSz);
                         HdrInSegment = true;
                       }
                     });
      for (uint64_t Offset : SectionOffsets)
        if (Offset > HF.Offset)
          HdrLimit = std::min(HdrLimit, Offset);

      uint64_t GnuOff = 0;
      forEachELFPhdr(Data, Size,
                     [&](const ELFPhdrFields &P, const uint8_t *Ptr, bool) {
                       if (GnuOff == 0 &&
                           P.Type == llvm::ELF::PT_GNU_EH_FRAME)
                         GnuOff = static_cast<uint64_t>(Ptr - Data);
                     });

      if (HdrInSegment && GnuOff != 0 && HdrLimit >= HF.Offset + HF.Size) {
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
  for (const llvm::Function &Function : Mod) {
    if (Function.isDeclaration())
      continue;
    if (Function.hasPersonalityFn())
      return true;
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block)
        if (llvm::isa<llvm::InvokeInst, llvm::LandingPadInst, llvm::ResumeInst>(
                Instruction))
          return true;
  }
  return false;
}

llvm::Error installELFEHFrame(std::vector<uint8_t> &Binary,
                              const std::optional<ELFEHFrameRegion> &Region,
                              const CompiledImage &Compiled,
                              const llvm::Module &Mod) {
  const CompiledSection *Generated = nullptr;
  for (const CompiledSection &Section : Compiled.Sections)
    if (Section.IsAllocated && Section.Name == section_names::elf::EhFrame) {
      Generated = &Section;
      break;
    }

  // A generated section left inside the patch image lives in the appended
  // segment, which the original `PT_GNU_EH_FRAME` does not cover, so it cannot
  // register.  With no exception contract that is fine; with one it is fatal.
  if (!Generated || Generated->IsInImage) {
    if (requiresRegisteredELFEHFrame(Mod))
      return patchError("no registrable .eh_frame produced");
    return llvm::Error::success();
  }

  const bool Registered =
      Region && installRecordsAndTable(Binary, *Region, *Generated);
  if (!Registered && requiresRegisteredELFEHFrame(Mod))
    return patchError("cannot register regenerated .eh_frame");

  LLVM_DEBUG({
    if (!Registered)
      llvm::dbgs() << "elf exception patch: omitting unregistered CFI-only "
                      ".eh_frame records\n";
  });
  return llvm::Error::success();
}

} // namespace neverd
