//===- ItaniumEH.cpp - Itanium exception recovery driver ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/DWARF/ItaniumEH.h"

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>

#define DEBUG_TYPE "neverd-dwarf-eh"

namespace neverd::dwarf_eh {
namespace {

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
  return false;
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
  if (Bases.Data == 0)
    if (const Section *Hdr =
            Img.getSectionByName(section_names::elf::EhFrameHdr))
      Bases.Data = Hdr->VA;
  return Bases;
}

/// Classify the exceptional shape of a decoded LSDA so the IR layers do not
/// each have to re-derive it from the action chain.
void summarizeItanium(ExceptionFunction &F) {
  if (!F.Itanium)
    return;
  const ItaniumEHInfo &Info = *F.Itanium;
  bool AnyLandingPad = false;
  for (const ItaniumCallSite &Site : Info.CallSites)
    AnyLandingPad = AnyLandingPad || Site.LandingPadVA != 0;
  if (!AnyLandingPad && Info.Actions.empty())
    F.Diagnostics.emplace_back(
        "Itanium record declares no landing pad: the frame only forwards");
}

} // namespace

void parseItaniumExceptions(BinaryImage &Img) {
  FrameSection Sec;
  if (!findFrameSection(Img, Sec) || Sec.Size == 0)
    return;

  const PointerBases Bases = computeBases(Img);
  ParseResult Frames = parseEHFrame(Img, Sec, Bases);
  if (Frames.FDEs.empty() && Frames.CIEs.empty())
    return;

  ExceptionInfo &Out = Img.ExceptionMetadata;
  Out.addModel(ExceptionModel::Itanium);
  Out.ParseStatus =
      mergeExceptionParseStatus(Out.ParseStatus, Frames.ParseStatus);
  for (std::string &Diag : Frames.Diagnostics)
    Out.Diagnostics.push_back(std::move(Diag));

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

  for (DwarfFDE &FDE : Frames.FDEs) {
    const DwarfCIE *CIE = Out.findCIE(FDE.CIESectionOffset);
    if (!CIE)
      continue;

    ExceptionFunction F;
    F.Encoding = ExceptionEncoding::DwarfFDE;
    F.Kind = RuntimeFunctionKind::Primary;
    F.UnwindInfoVA = Sec.VA + FDE.SectionOffset;

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
      F.ParseStatus = ExceptionParseStatus::Malformed;
      F.Diagnostics.emplace_back("DWARF FDE describes an invalid code range");
    }

    const Segment *Seg = Img.getSegmentFor(F.CodeRange.Begin);
    if (F.CodeRange.isValid() && (!Seg || !Seg->isExecutable())) {
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
      F.Diagnostics.emplace_back(
          "DWARF FDE range does not start in executable data");
    }

    F.PersonalityVA = CIE->PersonalityVA;
    if (CIE->PersonalityVA != 0 || CIE->PersonalitySlotVA != 0) {
      F.PersonalityName =
          resolveRoutineName(Img, CIE->PersonalityVA, CIE->PersonalitySlotVA);
      F.Personality = classifyPersonalityName(F.PersonalityName);
      if (F.Personality == ExceptionPersonality::Unknown) {
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Partial);
        F.Diagnostics.emplace_back("unknown Itanium personality routine");
      }
    }

    F.HandlerDataVA = FDE.LSDAVA;
    if (FDE.LSDAVA != 0) {
      LSDAParseRequest Req;
      Req.LSDAVA = FDE.LSDAVA;
      Req.FunctionStart = F.CodeRange.Begin;
      Req.FunctionEnd = F.CodeRange.End;
      Req.IsSJLJ = isSJLJPersonality(F.Personality);
      PointerBases LSDABases = Bases;
      LSDABases.Func = F.CodeRange.Begin;
      LSDAParseResult LSDA = parseLSDA(Img, Req, LSDABases);
      F.ParseStatus =
          mergeExceptionParseStatus(F.ParseStatus, LSDA.ParseStatus);
      for (std::string &Diag : LSDA.Diagnostics)
        F.Diagnostics.push_back(std::move(Diag));
      if (LSDA.Info)
        F.Itanium = std::move(*LSDA.Info);
      summarizeItanium(F);
    }

    F.Dwarf = std::move(FDE);
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus, F.ParseStatus);

    if (F.CodeRange.isValid()) {
      Img.KnownCodeRanges.emplace_back(F.CodeRange.Begin, F.CodeRange.End);
      // An FDE is an authoritative function boundary, which is stronger
      // evidence than the .eh_frame_hdr search table alone: the table only
      // holds the entry point, while the FDE also proves the extent.
      if (Seg && Seg->isExecutable() &&
          F.ParseStatus != ExceptionParseStatus::Malformed &&
          SeenSymbols.insert(F.CodeRange.Begin).second) {
        Img.Symbols.push_back(
            Symbol::makeFunc(F.CodeRange.Begin, F.CodeRange.size()));
        ++Added;
      }
    }
    Out.Functions.push_back(std::move(F));
  }

  if (Unrelocated != 0) {
    Out.ParseStatus =
        mergeExceptionParseStatus(Out.ParseStatus, ExceptionParseStatus::Partial);
    Out.Diagnostics.push_back(
        std::to_string(Unrelocated) +
        " DWARF FDEs name their functions through relocations the object has "
        "not had applied, so the frames they describe were left unattributed");
  }

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
  Out.rebuildIndex();

  LLVM_DEBUG(llvm::dbgs() << "dwarf-eh: normalized " << Frames.FDEs.size()
                          << " FDEs (" << Added << " new funcs)\n");
}

} // namespace neverd::dwarf_eh
