//===- MachOExceptions.cpp - Darwin exception recovery driver -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOExceptions.h"

#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>

#define DEBUG_TYPE "neverd-macho-unwind"

namespace neverd::macho_unwind {
namespace {

/// Locate `__eh_frame`, which a Darwin image only carries for the frames the
/// compact encodings cannot describe.
bool findEHFrame(const BinaryImage &Img, dwarf_eh::FrameSection &Out) {
  const Section *Sec = Img.getSectionByName(section_names::macho::EhFrame);
  if (!Sec || Sec->Size == 0)
    return false;
  if (!Sec->Data.empty()) {
    Out.Data = Sec->Data.data();
    Out.Size =
        std::min<size_t>(Sec->Data.size(), static_cast<size_t>(Sec->Size));
  } else {
    Out.Data = Img.readVA(Sec->VA, static_cast<size_t>(Sec->Size));
    Out.Size = Out.Data ? static_cast<size_t>(Sec->Size) : 0;
  }
  Out.VA = Sec->VA;
  return Out.Data != nullptr && Out.Size != 0;
}

dwarf_eh::PointerBases computeBases(const BinaryImage &Img) {
  dwarf_eh::PointerBases Bases;
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    if (Bases.Text == 0 || Seg.VA < Bases.Text)
      Bases.Text = Seg.VA;
  }
  if (const Section *Got = Img.getSectionByName(section_names::macho::Got))
    Bases.Data = Got->VA;
  return Bases;
}

/// Attach a decoded LSDA to \p F, recording whatever the record proved.
void attachLSDA(const BinaryImage &Img, const dwarf_eh::PointerBases &Bases,
                va_t LSDAVA, ExceptionFunction &F) {
  if (LSDAVA == 0)
    return;
  dwarf_eh::LSDAParseRequest Req;
  Req.LSDAVA = LSDAVA;
  Req.FunctionStart = F.CodeRange.Begin;
  Req.FunctionEnd = F.CodeRange.End;
  Req.Personality = F.Personality;
  Req.IsSJLJ = isSJLJPersonality(F.Personality);
  dwarf_eh::PointerBases LSDABases = Bases;
  LSDABases.Func = F.CodeRange.Begin;

  dwarf_eh::LSDAParseResult LSDA = dwarf_eh::parseLSDA(Img, Req, LSDABases);
  F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus, LSDA.ParseStatus);
  for (std::string &Diag : LSDA.Diagnostics)
    F.Diagnostics.push_back(std::move(Diag));
  if (LSDA.Info)
    F.Itanium = std::move(*LSDA.Info);
}

} // namespace

void parseDarwinExceptions(BinaryImage &Img) {
  ParseResult Compact = parseCompactUnwind(Img);

  dwarf_eh::FrameSection FrameSec;
  const bool HasFrames = findEHFrame(Img, FrameSec);
  const dwarf_eh::PointerBases Bases = computeBases(Img);

  dwarf_eh::ParseResult Frames;
  if (HasFrames)
    Frames = dwarf_eh::parseEHFrame(Img, FrameSec, Bases);

  if (Compact.Entries.empty() && Frames.FDEs.empty())
    return;

  ExceptionInfo &Out = Img.ExceptionMetadata;
  Out.ParseStatus =
      mergeExceptionParseStatus(Out.ParseStatus, Compact.ParseStatus);
  Out.ParseStatus =
      mergeExceptionParseStatus(Out.ParseStatus, Frames.ParseStatus);
  for (std::string &Diag : Compact.Diagnostics)
    Out.Diagnostics.push_back(std::move(Diag));
  for (std::string &Diag : Frames.Diagnostics)
    Out.Diagnostics.push_back(std::move(Diag));
  if (!Compact.Entries.empty())
    Out.addModel(ExceptionModel::CompactUnwind);
  if (!Frames.FDEs.empty())
    Out.addModel(ExceptionModel::Itanium);

  for (DwarfCIE &CIE : Frames.CIEs)
    if (!Out.findCIE(CIE.SectionOffset))
      Out.CIEs.push_back(CIE);

  // A compact entry in DWARF mode names its FDE by section offset, and an FDE
  // that has no compact entry at all must still be recorded.
  std::map<uint64_t, size_t> FDEByOffset;
  std::map<va_t, size_t> FDEByAddress;
  for (size_t I = 0; I < Frames.FDEs.size(); ++I) {
    FDEByOffset[Frames.FDEs[I].SectionOffset] = I;
    FDEByAddress[Frames.FDEs[I].InitialLocation] = I;
  }
  std::vector<bool> FDEConsumed(Frames.FDEs.size(), false);

  // Resolving a personality routine to a name is the expensive part and the
  // same handful of routines recur across every entry, so cache by address.
  std::map<va_t, std::pair<std::string, ExceptionPersonality>> NameCache;
  auto resolvePersonality = [&](va_t Routine, va_t Slot, std::string &Name,
                                ExceptionPersonality &Personality) {
    const va_t Key = Routine != 0 ? Routine : Slot;
    if (Key == 0)
      return;
    auto It = NameCache.find(Key);
    if (It == NameCache.end()) {
      std::string Resolved = resolveRoutineName(Img, Routine, Slot);
      It = NameCache
               .emplace(Key, std::make_pair(Resolved,
                                            classifyPersonalityName(Resolved)))
               .first;
    }
    Name = It->second.first;
    Personality = It->second.second;
  };

  auto recordFunctionRange = [&](const ExceptionAddressRange &Range) {
    if (!Range.isValid())
      return;
    Img.KnownCodeRanges.emplace_back(Range.Begin, Range.End);
    const Segment *Seg = Img.getSegmentFor(Range.Begin);
    if (!Seg || !Seg->isExecutable())
      return;

    bool Found = false;
    for (Symbol &Sym : Img.Symbols) {
      if (Sym.Addr != Range.Begin)
        continue;
      Found = true;
      Sym.IsFunc = true;
      if (Sym.Size == 0)
        Sym.Size = Range.size();
    }
    if (!Found)
      Img.Symbols.push_back(Symbol::makeFunc(Range.Begin, Range.size()));
  };

  for (const CompactUnwindEntry &Entry : Compact.Entries) {
    ExceptionFunction F;
    F.Encoding = ExceptionEncoding::CompactUnwind;
    F.Kind = RuntimeFunctionKind::Primary;
    F.CodeRange = Entry.CodeRange;
    F.Compact = Entry;
    F.HandlerDataVA = Entry.LSDAVA;

    if (Entry.SemanticStatus == CompactUnwindSemanticStatus::Partial) {
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
      if (Img.Arch == Arch::ARM &&
          (Entry.NativeEncoding & kModeMask) == kARMModeFrameD &&
          Entry.Kind == CompactUnwindKind::FramePointer) {
        F.Diagnostics.emplace_back(
            "compact unwind register layout depends on runtime stack "
            "alignment");
      } else {
        F.Diagnostics.emplace_back(
            "compact unwind encoding semantics are only partially decoded");
      }
    }

    if (Entry.Kind == CompactUnwindKind::Unknown) {
      F.ParseStatus = ExceptionParseStatus::Partial;
      F.Diagnostics.emplace_back("unrecognized compact unwind encoding");
    }

    // The frameless-indirect form exists precisely because the frame was too
    // large to encode inline, so an unresolved size is never a small one and
    // must not be read as zero by whatever consumes the frame layout.
    if (Entry.Kind == CompactUnwindKind::FramelessIndirect &&
        !Entry.HasStackSize) {
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
      F.Diagnostics.emplace_back(
          "compact unwind frame size is not readable from the prologue");
    }

    // The personality index is an index into the section's array; recover the
    // slot alongside the routine so a dynamically bound one can be named.
    if (Entry.PersonalityVA != 0 || (Entry.NativeEncoding & kPersonalityMask)) {
      const uint32_t Index =
          (Entry.NativeEncoding & kPersonalityMask) >> kPersonalityShift;
      const va_t Slot = Index != 0 && Index <= Compact.PersonalitySlots.size()
                            ? Compact.PersonalitySlots[Index - 1]
                            : 0;
      F.PersonalityVA = Entry.PersonalityVA;
      resolvePersonality(Entry.PersonalityVA, Slot, F.PersonalityName,
                         F.Personality);
      if (F.Personality == ExceptionPersonality::Unknown) {
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Partial);
        F.Diagnostics.emplace_back("unknown compact unwind personality");
      }
    }

    if (Entry.Kind == CompactUnwindKind::DwarfFDE) {
      auto It = FDEByOffset.find(Entry.DwarfFDEOffset);
      if (It != FDEByOffset.end()) {
        F.Dwarf = Frames.FDEs[It->second];
        FDEConsumed[It->second] = true;
        if (F.HandlerDataVA == 0)
          F.HandlerDataVA = F.Dwarf->LSDAVA;
      } else {
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Partial);
        F.Diagnostics.emplace_back(
            "compact unwind entry names an FDE that was not decoded");
      }
    } else if (auto It = FDEByAddress.find(F.CodeRange.Begin);
               It != FDEByAddress.end()) {
      // A frame can carry both: the compact entry describes the prologue and
      // the FDE the full rule set.  Keeping both loses nothing.
      F.Dwarf = Frames.FDEs[It->second];
      FDEConsumed[It->second] = true;
    }

    attachLSDA(Img, Bases, F.HandlerDataVA, F);
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus, F.ParseStatus);

    recordFunctionRange(F.CodeRange);
    Out.Functions.push_back(std::move(F));
  }

  // FDEs with no compact entry: rare, but they exist for assembly routines the
  // linker could not summarize.
  for (size_t I = 0; I < Frames.FDEs.size(); ++I) {
    if (FDEConsumed[I])
      continue;
    DwarfFDE &FDE = Frames.FDEs[I];
    const DwarfCIE *CIE = Out.findCIE(FDE.CIESectionOffset);
    if (!CIE)
      continue;

    ExceptionFunction F;
    F.Encoding = ExceptionEncoding::DwarfFDE;
    F.Kind = RuntimeFunctionKind::Primary;
    F.UnwindInfoVA = FrameSec.VA + FDE.SectionOffset;
    if (auto Range = ExceptionAddressRange::fromStartAndSize(
            FDE.InitialLocation, FDE.AddressRange))
      F.CodeRange = *Range;
    F.PersonalityVA = CIE->PersonalityVA;
    if (CIE->PersonalityVA != 0 || CIE->PersonalitySlotVA != 0) {
      F.PersonalityName =
          resolveRoutineName(Img, CIE->PersonalityVA, CIE->PersonalitySlotVA);
      F.Personality = classifyPersonalityName(F.PersonalityName);
    }
    F.HandlerDataVA = FDE.LSDAVA;
    attachLSDA(Img, Bases, FDE.LSDAVA, F);
    F.Dwarf = std::move(FDE);
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus, F.ParseStatus);
    recordFunctionRange(F.CodeRange);
    Out.Functions.push_back(std::move(F));
  }

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
  Out.rebuildIndex();

  LLVM_DEBUG(llvm::dbgs() << "macho-unwind: normalized " << Out.Functions.size()
                          << " exception records\n");
}

} // namespace neverd::macho_unwind
