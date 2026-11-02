//===- ItaniumEH.cpp - Itanium exception recovery driver ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/DWARF/ItaniumEH.h"

#include "EHFrameDetail.h"

#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/ELF/ELFImageMetadata.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/Limits.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <optional>

#define DEBUG_TYPE "neverd-dwarf-eh"

namespace neverd::dwarf_eh {
namespace {

/// Locate .eh_frame the way an ELF image's unwinder does, which needs no
/// section header: PT_GNU_EH_FRAME locates .eh_frame_hdr, its eh_frame_ptr
/// says where .eh_frame begins, and its records run to a zero length.
bool findFrameTableFromProgramHeaders(const BinaryImage &Img,
                                      FrameSection &Out) {
  using namespace dweh;
  if (!Img.ELFMetadata)
    return false;
  for (const ELFProgramHeader &P : Img.ELFMetadata->ProgramHeaders) {
    if (P.Type != llvm::ELF::PT_GNU_EH_FRAME)
      continue;
    const size_t HeaderSize =
        static_cast<size_t>(std::min<uint64_t>(P.FileSize, kEhFrameHdrMinSize));
    const uint8_t *Header = Img.readVA(P.VirtualAddress, HeaderSize);
    if (!Header || HeaderSize < kEhFrameHdrMinSize ||
        Header[0] != kEhFrameHdrVersion)
      return false;
    PointerBases Bases;
    Bases.Data = P.VirtualAddress;
    size_t Cursor = sizeof(EhFrameHdrHeader);
    va_t FrameVA = 0;
    if (!readEncodedPointer(Header, HeaderSize, Cursor, P.VirtualAddress,
                            Header[1], Bases, Img.getPointerSize(), &Img,
                            FrameVA))
      return false;
    va_t End = FrameVA;
    for (;;) {
      const uint8_t *Length = Img.readVA(End, kRecordLengthSize);
      if (!Length)
        break;
      uint32_t Word;
      std::memcpy(&Word, Length, sizeof(Word));
      if (Word == 0) {
        End += kRecordLengthSize;
        break;
      }
      uint64_t RecordSize = kRecordLengthSize + uint64_t(Word);
      if (Word == kDwarf64LengthEscape) {
        const uint8_t *Long =
            Img.readVA(End + kRecordLengthSize, kDwarf64LengthSize);
        if (!Long)
          break;
        uint64_t LongLength;
        std::memcpy(&LongLength, Long, sizeof(LongLength));
        RecordSize = kRecordLengthSize + kDwarf64LengthSize + LongLength;
      }
      if (RecordSize > std::numeric_limits<size_t>::max() ||
          !Img.readVA(End, static_cast<size_t>(RecordSize)))
        break;
      End += RecordSize;
    }
    if (End == FrameVA)
      return false;
    const uint8_t *Bytes =
        Img.readVA(FrameVA, static_cast<size_t>(End - FrameVA));
    if (!Bytes)
      return false;
    Out.Data = Bytes;
    Out.Size = static_cast<size_t>(End - FrameVA);
    Out.VA = FrameVA;
    return true;
  }
  return false;
}

/// Locate the call frame section and where it is mapped.
///
/// Section bytes are preferred over segment bytes because a relocatable
/// object has sections but no load segments, and because a section carries
/// the exact size the producer emitted.
bool findFrameSection(const BinaryImage &Img, FrameSection &Out) {
  const char *Names[] = {section_names::elf::EhFrame,
                         section_names::macho::EhFrame};
  for (const char *Name : Names) {
    const Section *Sec = Img.getSectionByName(Name);
    if (!Sec || Sec->Size == 0)
      continue;
    if (!Sec->Data.empty()) {
      Out.Data = Sec->Data.data();
      Out.Size =
          std::min<size_t>(Sec->Data.size(), static_cast<size_t>(Sec->Size));
      Out.VA = Sec->VA;
      return true;
    }
    // A section whose bytes were not retained can still be read through the
    // segment it was mapped into.
    if (Sec->VA != 0 && Sec->Size <= std::numeric_limits<size_t>::max()) {
      if (const uint8_t *Bytes =
              Img.readVA(Sec->VA, static_cast<size_t>(Sec->Size))) {
        Out.Data = Bytes;
        Out.Size = static_cast<size_t>(Sec->Size);
        Out.VA = Sec->VA;
        return true;
      }
    }
  }
  return findFrameTableFromProgramHeaders(Img, Out);
}

PointerBases computeBases(const BinaryImage &Img) {
  PointerBases Bases;
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    if (Bases.Text == 0 || Seg.VA < Bases.Text)
      Bases.Text = Seg.VA;
  }
  // The psABI resolves DW_EH_PE_datarel inside .eh_frame_hdr against that
  // section, and elsewhere against the GOT.  Prefer the GOT because a
  // datarel pointer inside .eh_frame is the case being decoded here.
  for (const char *Name : {section_names::elf::GotPlt, section_names::elf::Got,
                           section_names::macho::Got}) {
    if (const Section *Sec = Img.getSectionByName(Name)) {
      Bases.Data = Sec->VA;
      break;
    }
  }
  // Without section headers the GOT is where DT_PLTGOT says, and
  // .eh_frame_hdr where PT_GNU_EH_FRAME does.
  if (Bases.Data == 0)
    Bases.Data = Img.DynInfo.PltGotAddr;
  if (Bases.Data == 0)
    if (const Section *Hdr =
            Img.getSectionByName(section_names::elf::EhFrameHdr))
      Bases.Data = Hdr->VA;
  if (Bases.Data == 0 && Img.ELFMetadata)
    for (const ELFProgramHeader &P : Img.ELFMetadata->ProgramHeaders)
      if (P.Type == llvm::ELF::PT_GNU_EH_FRAME) {
        Bases.Data = P.VirtualAddress;
        break;
      }
  return Bases;
}

/// Runtime indices may name additional FDEs outside the original section.
/// Decode their actual CIE/FDE storage rather than assigning bounds from the
/// search keys alone. Offsets remain relative to the primary frame section so
/// existing consumers have one stable CIE identity namespace.
void appendIndexedELFFrames(const BinaryImage &Img, const FrameSection &Sec,
                            const PointerBases &Bases, ParseResult &Frames) {
  if (Img.IsRelocatable || !Img.ELFMetadata)
    return;
  auto Malformed = [&](const char *Message) {
    Frames.ParseStatus = mergeExceptionParseStatus(
        Frames.ParseStatus, ExceptionParseStatus::Malformed);
    Frames.Diagnostics.emplace_back(Message);
  };
  const ELFProgramHeader *Header = nullptr;
  for (const auto &P : Img.ELFMetadata->ProgramHeaders) {
    if (P.Type != llvm::ELF::PT_GNU_EH_FRAME)
      continue;
    if (Header) {
      Malformed("multiple runtime DWARF frame indices");
      return;
    }
    Header = &P;
  }
  if (!Header)
    return;
  const auto Size = Header->FileSize;
  const auto VA = Header->VirtualAddress;
  const uint8_t *Bytes =
      Size >= 4 && Size <= ::neverd::limits::loader::kItaniumEHMaxIndex
          ? Img.readVA(VA, Size)
          : nullptr;
  if (!Bytes || Bytes[0] != 1) {
    Malformed("invalid runtime DWARF frame index");
    return;
  }
  // An omitted search table is legal; the primary frame section remains the
  // only discoverable source in that profile.
  if (Bytes[2] == dweh::Omit || Bytes[3] == dweh::Omit)
    return;
  PointerBases HeaderBases = Bases;
  HeaderBases.Data = VA;
  size_t Cursor = 4;
  va_t FrameVA = 0, Count = 0;
  if (!readEncodedPointer(Bytes, Size, Cursor, VA, Bytes[1], HeaderBases,
                          Img.getPointerSize(), &Img, FrameVA) ||
      !readEncodedPointer(Bytes, Size, Cursor, VA, Bytes[2], {},
                          Img.getPointerSize(), nullptr, Count) ||
      Count > (1u << 20)) {
    Malformed("unreadable runtime DWARF frame index header");
    return;
  }
  std::map<va_t, size_t> Known;
  for (size_t I = 0; I < Frames.FDEs.size(); ++I)
    Known.emplace(Sec.VA + Frames.FDEs[I].SectionOffset, I);
  uint64_t Budget = ::neverd::limits::loader::kItaniumEHMaxDecode;
  va_t Previous = 0;
  for (uint64_t I = 0; I < Count; ++I) {
    va_t Start = 0, FDEVA = 0;
    if (!readEncodedPointer(Bytes, Size, Cursor, VA, Bytes[3], HeaderBases,
                            Img.getPointerSize(), &Img, Start) ||
        !readEncodedPointer(Bytes, Size, Cursor, VA, Bytes[3], HeaderBases,
                            Img.getPointerSize(), &Img, FDEVA) ||
        (I && Start <= Previous)) {
      Malformed("invalid or unsorted runtime DWARF frame index entry");
      return;
    }
    Previous = Start;
    if (auto Found = Known.find(FDEVA); Found != Known.end()) {
      if (Frames.FDEs[Found->second].InitialLocation != Start)
        Malformed("runtime DWARF index key disagrees with its FDE");
      continue;
    }
    const uint8_t *Prefix = Img.readVA(FDEVA, 8);
    if (!Prefix) {
      Malformed("runtime DWARF index names an unreadable FDE");
      continue;
    }
    const uint32_t Word = readLE<uint32_t>(Prefix);
    uint64_t Length = Word, IDOffset = 4, IDSize = 4;
    if (Word == UINT32_MAX) {
      Prefix = Img.readVA(FDEVA, 20);
      if (!Prefix) {
        Malformed("runtime DWARF index names a truncated DWARF64 FDE");
        continue;
      }
      Length = readLE<uint64_t>(Prefix + 4);
      IDOffset = 12;
      IDSize = 8;
    }
    const uint64_t Distance = IDSize == 8 ? readLE<uint64_t>(Prefix + IDOffset)
                                          : readLE<uint32_t>(Prefix + IDOffset);
    if (Length < IDSize || Length > Budget || FDEVA > UINT64_MAX - IDOffset ||
        !Distance || Distance > FDEVA + IDOffset ||
        FDEVA + IDOffset - Distance < Sec.VA) {
      Malformed("runtime DWARF index has an invalid FDE/CIE extent");
      continue;
    }
    const va_t CIEVA = FDEVA + IDOffset - Distance;
    if (Distance > Budget - Length) {
      Malformed("indexed DWARF records exceed the decode budget");
      return;
    }
    const uint64_t Span = Distance + Length;
    const uint8_t *Storage = Img.readVA(CIEVA, Span);
    if (!Storage) {
      Malformed("indexed DWARF CIE and FDE have no complete backing range");
      continue;
    }
    Budget -= Span;
    auto Added = parseEHFrame(Img, {Storage, size_t(Span), CIEVA}, Bases);
    auto Found =
        std::find_if(Added.FDEs.begin(), Added.FDEs.end(), [&](const auto &F) {
          return CIEVA + F.SectionOffset == FDEVA && F.InitialLocation == Start;
        });
    if (Added.ParseStatus != ExceptionParseStatus::Complete ||
        Found == Added.FDEs.end()) {
      Malformed(
          "indexed DWARF records do not decode to their declared identity");
      continue;
    }
    const uint64_t Shift = CIEVA - Sec.VA;
    for (auto &CIE : Added.CIEs) {
      CIE.SectionOffset += Shift;
      if (llvm::none_of(Frames.CIEs, [&](const auto &Old) {
            return Old.SectionOffset == CIE.SectionOffset;
          }))
        Frames.CIEs.push_back(std::move(CIE));
    }
    auto FDE = std::move(*Found);
    FDE.SectionOffset += Shift;
    FDE.CIESectionOffset += Shift;
    FDE.InitialLocationOffset += Shift;
    Known.emplace(FDEVA, Frames.FDEs.size());
    Frames.FDEs.push_back(std::move(FDE));
  }
}

/// Classify the exceptional shape of a decoded LSDA so the IR layers do not
/// each have to re-derive it from the action chain.
void summarizeItanium(const ItaniumEHInfo &Info,
                      ExceptionDecodeState &LanguageDecode) {
  bool AnyLandingPad = false;
  for (const ItaniumCallSite &Site : Info.CallSites)
    AnyLandingPad = AnyLandingPad || Site.LandingPadVA != 0;
  if (!AnyLandingPad && Info.Actions.empty())
    LanguageDecode.Diagnostics.emplace_back(
        "Itanium record declares no landing pad: the frame only forwards");
}

/// Where the function whose frame begins at \p FrameBegin starts.
///
/// bionic's AArch64 assembly entry macro puts `bti c` before
/// `.cfi_startproc`, so the frame of each such function -- every syscall
/// stub -- begins at its second instruction. A call landing pad directly
/// before a frame, which no frame covers, is that function's first
/// instruction.
va_t functionEntry(const BinaryImage &Img, va_t FrameBegin,
                   const std::vector<std::pair<va_t, va_t>> &Frames) {
  if (Img.Arch != Arch::AArch64 || FrameBegin < aarch64::kInsnSize ||
      FrameBegin % aarch64::kInsnSize != 0)
    return FrameBegin;
  const va_t Pad = FrameBegin - aarch64::kInsnSize;
  const auto Next = std::upper_bound(Frames.begin(), Frames.end(),
                                     std::make_pair(Pad, InvalidVA));
  if (Next != Frames.begin() && Pad < std::prev(Next)->second)
    return FrameBegin;
  const Segment *Seg = Img.getSegmentFor(Pad);
  if (!Seg || !Seg->isExecutable() || Pad < Seg->VA ||
      FrameBegin - Seg->VA > Seg->Data.size())
    return FrameBegin;
  const uint32_t Word = readLE<uint32_t>(Seg->Data.data() + (Pad - Seg->VA));
  return Word == aarch64::kBTI_C || Word == aarch64::kBTI_JC ? Pad : FrameBegin;
}

} // namespace

void recordFrameExtents(BinaryImage &Img) {
  FrameSection Sec;
  if (Img.IsRelocatable || !findFrameSection(Img, Sec) || Sec.Size == 0)
    return;
  const auto Bases = computeBases(Img);
  // Discovery uses the encoded extents, even when a frame's CFI is malformed.
  // Decode the programs later, when exception recovery consumes their rules.
  // Indexed extension records retain the full validation their admission needs.
  ParseResult Frames = detail::parseFrameHeaders(Img, Sec, Bases);
  appendIndexedELFFrames(Img, Sec, Bases, Frames);
  std::vector<std::pair<va_t, va_t>> Ranges;
  Ranges.reserve(Frames.FDEs.size());
  for (const DwarfFDE &FDE : Frames.FDEs) {
    const va_t Begin = Img.Arch == Arch::ARM
                           ? clearThumbBit(FDE.InitialLocation)
                           : FDE.InitialLocation;
    if (auto Range =
            ExceptionAddressRange::fromStartAndSize(Begin, FDE.AddressRange))
      Ranges.emplace_back(Range->Begin, Range->End);
  }
  std::sort(Ranges.begin(), Ranges.end());
  // Functions known only by an automatic name and an entry.
  llvm::DenseMap<va_t, size_t> Unsized;
  for (size_t Index = 0; Index < Img.Symbols.size(); ++Index) {
    const Symbol &Sym = Img.Symbols[Index];
    if (Sym.IsFunc && Sym.Size == 0 &&
        Sym.Name == Symbol::makeFunc(Sym.Addr).Name)
      Unsized.try_emplace(Sym.Addr, Index);
  }
  for (const auto &[Begin, End] : Ranges) {
    const Segment *Seg = Img.getSegmentFor(Begin);
    if (!Seg || !Seg->isExecutable())
      continue;
    const va_t Entry = functionEntry(Img, Begin, Ranges);
    Img.KnownCodeRanges.emplace_back(Entry, End);
    // A frame that starts after its function's landing pad leaves the entry
    // named at the frame start to parseItaniumExceptions, which removes it.
    if (Entry != Begin)
      continue;
    // A linker's PLT frame covers every stub of its section, not a function.
    if (const Section *Stubs = Img.getSectionFor(Begin)) {
      const llvm::StringRef Name = Stubs->Name;
      if (Name == section_names::elf::Plt || Name == section_names::elf::Iplt ||
          Name.starts_with(section_names::elf::PltPrefix))
        continue;
    }
    if (auto It = Unsized.find(Begin); It != Unsized.end())
      Img.Symbols[It->second].Size = End - Begin;
  }
  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
}

void parseItaniumExceptions(BinaryImage &Img) {
  FrameSection Sec;
  if (!findFrameSection(Img, Sec) || Sec.Size == 0)
    return;

  const PointerBases Bases = computeBases(Img);
  ParseResult Frames = parseEHFrame(Img, Sec, Bases);
  appendIndexedELFFrames(Img, Sec, Bases, Frames);
  if (Frames.FDEs.empty() && Frames.CIEs.empty())
    return;

  ExceptionInfo &Out = Img.ExceptionMetadata;
  Out.addModel(ExceptionModel::Itanium);
  ExceptionDecodeState &ImageStructural = Out.structuralDecodeState();
  ImageStructural.mergeStatus(Frames.ParseStatus);
  ImageStructural.appendDiagnostics(Frames.Diagnostics);

  // CIEs are shared; keep one copy per section offset in the image record.
  const size_t CIEBase = Out.CIEs.size();
  (void)CIEBase;
  for (DwarfCIE &CIE : Frames.CIEs)
    if (!Out.findCIE(CIE.SectionOffset))
      Out.CIEs.push_back(CIE);

  auto SeenSymbols = Img.getSymbolAddresses();
  size_t Added = 0;
  size_t Unrelocated = 0;

  // In a relocatable object every FDE names its function through a relocation,
  // and the field holds a displacement from a section base the link step has
  // not chosen yet.  Reading it as an address puts every FDE on whatever
  // happens to sit at the bottom of the synthesized layout, which then invents
  // function symbols there and reshapes the analysis around them.  Collecting
  // the offsets a relocation covers lets each FDE say whether its own location
  // was written down or is still owed.
  //
  // A relocation in an unlinked object records a section-relative offset, not
  // an address, and names the relocation section it came from -- `.rela` or
  // `.rel` prefixed onto the section it applies to.
  llvm::DenseSet<uint64_t> RelocatedOffsets;
  if (Img.IsRelocatable) {
    llvm::StringRef FrameName;
    for (const Section &S : Img.Sections)
      if (S.VA == Sec.VA && !S.Name.empty())
        FrameName = S.Name;
    if (!FrameName.empty())
      for (const RelocationEntry &Rel : Img.Relocations)
        if (llvm::StringRef(Rel.SectionName).ends_with(FrameName))
          RelocatedOffsets.insert(Rel.Address);
  }

  // Every code range a frame describes, to tell a landing pad before one
  // frame from the last instruction of another.
  std::vector<std::pair<va_t, va_t>> FrameRanges;
  for (const DwarfFDE &FDE : Frames.FDEs) {
    if (RelocatedOffsets.contains(FDE.InitialLocationOffset))
      continue;
    const va_t Begin = Img.Arch == Arch::ARM
                           ? clearThumbBit(FDE.InitialLocation)
                           : FDE.InitialLocation;
    if (auto Range =
            ExceptionAddressRange::fromStartAndSize(Begin, FDE.AddressRange))
      FrameRanges.emplace_back(Range->Begin, Range->End);
  }
  std::sort(FrameRanges.begin(), FrameRanges.end());
  // These attributable ranges already prove how many ordinary records will
  // be materialized. Avoid repeatedly growing and moving the much larger
  // normalized records. Identifiable malformed ranges still get records below
  // and can grow the vector beyond this reservation.
  Out.Functions.reserve(Out.Functions.size() + FrameRanges.size());
  // Frames whose function starts at the landing pad before them.
  llvm::DenseSet<va_t> PaddedFrameStarts;
  // Most frames share a few CIEs, and naming a personality scans the image's
  // relocations and symbols.  The loop below only adds auto-named function
  // symbols, which never name a routine, so a name found once stays exact.
  std::map<std::pair<va_t, va_t>, std::string> PersonalityNames;

  for (DwarfFDE &FDE : Frames.FDEs) {
    const DwarfCIE *CIE = Out.findCIE(FDE.CIESectionOffset);
    if (!CIE)
      continue;

    ExceptionFunction F;
    F.Encoding = ExceptionEncoding::DwarfFDE;
    F.Kind = RuntimeFunctionKind::Primary;
    F.UnwindInfoVA = Sec.VA + FDE.SectionOffset;
    ExceptionFunctionDecodeProvenance &Decode = F.establishDecodeProvenance();

    if (RelocatedOffsets.contains(FDE.InitialLocationOffset)) {
      // A record that cannot say which function it describes is not a record
      // about a function.  Keeping it in the per-function table would let it
      // be matched to whatever the unrelocated field happens to point at,
      // which is exactly the frame it is not about.
      ++Unrelocated;
      continue;
    }

    va_t Begin = FDE.InitialLocation;
    if (Img.Arch == Arch::ARM)
      Begin = clearThumbBit(Begin);
    if (auto Range =
            ExceptionAddressRange::fromStartAndSize(Begin, FDE.AddressRange)) {
      F.CodeRange = *Range;
    } else {
      Decode.Structural.ParseStatus = ExceptionParseStatus::Malformed;
      Decode.Structural.Diagnostics.emplace_back(
          "DWARF FDE describes an invalid code range");
    }

    const Segment *Seg = Img.getSegmentFor(F.CodeRange.Begin);
    if (F.CodeRange.isValid() && (!Seg || !Seg->isExecutable())) {
      Decode.Structural.mergeStatus(ExceptionParseStatus::Partial);
      Decode.Structural.Diagnostics.emplace_back(
          "DWARF FDE range does not start in executable data");
    }

    F.PersonalityVA = CIE->PersonalityVA;
    if (CIE->PersonalityVA != 0 || CIE->PersonalitySlotVA != 0) {
      auto [Known, Inserted] = PersonalityNames.try_emplace(
          {CIE->PersonalityVA, CIE->PersonalitySlotVA});
      if (Inserted)
        Known->second =
            resolveRoutineName(Img, CIE->PersonalityVA, CIE->PersonalitySlotVA);
      F.PersonalityName = Known->second;
      F.Personality = F.PersonalityName.empty()
                          ? ExceptionPersonality::Unknown
                          : classifyPersonalityName(F.PersonalityName);
      if (F.Personality == ExceptionPersonality::Unknown) {
        Decode.Language.mergeStatus(ExceptionParseStatus::Partial);
        Decode.Language.Diagnostics.emplace_back(
            "unknown Itanium personality routine");
      }
    }

    F.HandlerDataVA = FDE.LSDAVA;
    if (FDE.LSDAVA != 0) {
      LSDAParseRequest Req;
      Req.LSDAVA = FDE.LSDAVA;
      Req.FunctionStart = F.CodeRange.Begin;
      Req.FunctionEnd = F.CodeRange.End;
      Req.Personality = F.Personality;
      Req.IsSJLJ = isSJLJPersonality(F.Personality);
      PointerBases LSDABases = Bases;
      LSDABases.Func = F.CodeRange.Begin;
      LSDAParseResult LSDA = parseLSDA(Img, Req, LSDABases);
      Decode.Language.mergeStatus(LSDA.ParseStatus);
      Decode.Language.appendDiagnostics(LSDA.Diagnostics);
      if (LSDA.Info)
        F.Itanium = std::move(*LSDA.Info);
      if (F.Itanium)
        summarizeItanium(*F.Itanium, Decode.Language);
    }

    F.Dwarf = std::move(FDE);
    inheritFrameSectionDecodeStatus(Frames, F);
    F.rebuildParseSummary();

    if (F.CodeRange.isValid()) {
      const va_t Entry = functionEntry(Img, F.CodeRange.Begin, FrameRanges);
      if (Entry != F.CodeRange.Begin) {
        PaddedFrameStarts.insert(F.CodeRange.Begin);
        F.FunctionEntry = Entry;
      }
      Img.KnownCodeRanges.emplace_back(Entry, F.CodeRange.End);
      // An FDE is an authoritative function boundary, which is stronger
      // evidence than the .eh_frame_hdr search table alone: the table only
      // holds the entry point, while the FDE also proves the extent.
      if (Seg && Seg->isExecutable() &&
          F.ParseStatus != ExceptionParseStatus::Malformed &&
          SeenSymbols.insert(Entry).second) {
        Img.Symbols.push_back(Symbol::makeFunc(Entry, F.CodeRange.End - Entry));
        ++Added;
      }
    }
    Out.Functions.push_back(std::move(F));
  }

  // .eh_frame_hdr's search table names each frame's first address, which the
  // ELF loader already made a function; where the function starts at the
  // landing pad before its frame, that address is its second instruction.
  if (!PaddedFrameStarts.empty())
    llvm::erase_if(Img.Symbols, [&](const Symbol &Sym) {
      return Sym.IsFunc && Sym.Size == 0 &&
             PaddedFrameStarts.contains(Sym.Addr) &&
             Sym.Name == Symbol::makeFunc(Sym.Addr).Name;
    });

  if (Unrelocated != 0) {
    ImageStructural.mergeStatus(ExceptionParseStatus::Partial);
    ImageStructural.Diagnostics.push_back(
        std::to_string(Unrelocated) +
        " DWARF FDEs name their functions through relocations the object has "
        "not had applied, so the frames they describe were left unattributed");
  }

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
  bindColdFragments(Img);
  Out.rebuildIndex();
  Out.rebuildParseSummary();

  LLVM_DEBUG(llvm::dbgs() << "dwarf-eh: normalized " << Frames.FDEs.size()
                          << " FDEs (" << Added << " new funcs)\n");
}

bool refreshItaniumLanguageData(const BinaryImage &Img,
                                ExceptionFunction &Function) {
  if (Function.Encoding != ExceptionEncoding::DwarfFDE || !Function.Dwarf ||
      !Function.DecodeProvenance || Function.HandlerDataVA == 0 ||
      !Function.CodeRange.isValid() || !Img.readVA(Function.HandlerDataVA, 1))
    return false;

  LSDAParseRequest Req;
  Req.LSDAVA = Function.HandlerDataVA;
  Req.FunctionStart = Function.CodeRange.Begin;
  Req.FunctionEnd = Function.CodeRange.End;
  Req.Personality = Function.Personality;
  Req.IsSJLJ = isSJLJPersonality(Function.Personality);

  PointerBases Bases = computeBases(Img);
  Bases.Func = Function.CodeRange.Begin;
  LSDAParseResult LSDA = parseLSDA(Img, Req, Bases);
  if (LSDA.ParseStatus != ExceptionParseStatus::Complete || !LSDA.Info)
    return false;

  ExceptionDecodeState LanguageDecode;
  LanguageDecode.appendDiagnostics(LSDA.Diagnostics);
  summarizeItanium(*LSDA.Info, LanguageDecode);

  Function.Itanium = std::move(*LSDA.Info);
  Function.DecodeProvenance->Language = std::move(LanguageDecode);
  Function.Rust.reset();
  Function.ObjC.reset();
  Function.rebuildParseSummary();
  return true;
}

} // namespace neverd::dwarf_eh
