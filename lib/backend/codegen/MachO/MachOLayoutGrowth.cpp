//===- MachOLayoutGrowth.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reserve native metadata without padding.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"

#include "MachOStrictLayout.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {
#define NEVERD_MACHO_FRAME_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define NEVERD_MACHO_FRAME_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/backend/codegen/MachO/MachOFrameRelocation.def"

llvm::Error fail(const llvm::Twine &S) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "Mach-O layout: " + S);
}
llvm::Expected<uint64_t> moveTextSection(std::vector<uint8_t> &Binary, Arch A,
                                         llvm::StringRef Name,
                                         llvm::ArrayRef<uint8_t> Content,
                                         uint64_t Capacity) {
  using namespace llvm::MachO;
  if (!Capacity ||
      Capacity > ::neverd::limits::backend::
                     kMachOLayoutGrowthMoveTextSectionCapacityLimit ||
      Content.size() > Capacity)
    return fail("metadata capacity exceeds its checked limit");
  // Newly mapped header-prefix capacity can host metadata without moving
  // code/data VAs or extending TEXT across an intervening DATA segment.
  const auto H = parseMachOHeader(Binary.data(), Binary.size());
  const uint64_t Off =
      llvm::alignTo(uint64_t(H.HeaderSize) + H.SizeOfCmds + 512, 8);
  uint64_t First = findFirstMachODataOffset(Binary.data(), Binary.size());
  uint64_t VA = 0;
  auto Probe = Binary;
  auto Prefix = reserveMachOHeaderStorage(Probe, A, 512 + Capacity);
  const bool OwnedPrefix = Prefix && !*Prefix;
  if (!Prefix)
    llvm::consumeError(Prefix.takeError());
  if (OwnedPrefix && Off <= First && Capacity <= First - Off &&
      rangeInBounds(Off, Capacity, Binary.size()) &&
      llvm::all_of(llvm::ArrayRef<uint8_t>(Binary).slice(Off, Capacity),
                   [](uint8_t V) { return !V; })) {
    forEachMachOLoadCommand(
        Binary.data(), Binary.size(),
        [&](const uint8_t *C, uint32_t Cmd, uint32_t, bool Is64) {
          if (Cmd != getMachOSegmentCmdID(Is64))
            return;
          const auto G = readMachOSegment(C, Is64);
          if (readMachOName(G.SegName) == "__TEXT" && !G.FileOff &&
              rangeInBounds(Off, Capacity, G.FileSize))
            VA = G.VMAddr + Off;
        });
    if (VA)
      std::copy(Content.begin(), Content.end(), Binary.begin() + Off);
  }
  if (!VA) {
    MachOPatcher P;
    P.setInputKind(RewriteInputKind::ReplacementIR);
    std::vector<uint8_t> Storage(Capacity, 0);
    std::copy(Content.begin(), Content.end(), Storage.begin());
    VA = P.appendExecSegment(Binary, Storage, "__TEXT", A);
  }
  if (!VA)
    return fail("metadata growth requires authenticated TEXT capacity");
  unsigned Count = 0;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *C, uint32_t Cmd, uint32_t, bool Is64) {
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;
        const auto F = readMachOSegment(C, Is64);
        if (readMachOName(F.SegName) != "__TEXT" || VA < F.VMAddr ||
            !rangeInBounds(VA - F.VMAddr, Capacity, F.FileSize))
          return;
        auto *Out =
            Binary.data() + (C - Binary.data()) + getMachOSegmentCmdSize(Is64);
        for (unsigned I = 0; I < F.NSects;
             ++I, Out += getMachOSectionSize(Is64)) {
          if (readMachOName(reinterpret_cast<const char *>(Out)) != Name)
            continue;
          const uint64_t Offset = F.FileOff + VA - F.VMAddr;
          if (Offset > UINT32_MAX ||
              (!Is64 && (VA > UINT32_MAX || Capacity > UINT32_MAX)))
            return;
          if (Is64) {
            auto *S = reinterpret_cast<section_64 *>(Out);
            S->addr = VA;
            S->offset = Offset;
            S->size = Capacity;
          } else {
            auto *S = reinterpret_cast<section *>(Out);
            S->addr = VA;
            S->offset = Offset;
            S->size = Capacity;
          }
          ++Count;
        }
      });
  std::vector<macho_patch_detail::MachOFileRange> Segments, Sections;
  if (Count != 1 ||
      !macho_patch_detail::collectMachOFileRanges(Binary, Segments, Sections))
    return fail("moved metadata lost its unique TEXT mapping");
  return VA;
}

bool hasRelocatableAugmentation(const DwarfCIE &CIE, Arch Architecture) {
  if (Architecture != Arch::AArch64)
    return CIE.Augmentation == kBasicAugmentation;
  llvm::StringRef Augmentation(CIE.Augmentation);
  if (Augmentation.empty() || Augmentation.front() != kAugmentationLength)
    return false;
  bool Encoding = false, BKey = false, NonHint = false;
  for (char Character : Augmentation.drop_front()) {
    bool *Seen = nullptr;
    switch (Character) {
    case kAugmentationEncoding:
      Seen = &Encoding;
      break;
    case dwarf_eh::aarch64::kBKeyAugmentation:
      Seen = &BKey;
      break;
    case dwarf_eh::aarch64::kNonHintAugmentation:
      Seen = &NonHint;
      break;
    default:
      return false;
    }
    if (*Seen)
      return false;
    *Seen = true;
  }
  return Encoding && BKey == CIE.HasPointerAuth &&
         NonHint == CIE.UsesNonHintPAuthLR;
}

bool sameCFI(llvm::ArrayRef<CFIInstruction> Before,
             llvm::ArrayRef<CFIInstruction> After) {
  return Before.size() == After.size() &&
         llvm::equal(Before, After, [](const auto &A, const auto &B) {
           return std::tie(A.Kind, A.CodeOffset, A.Register, A.Register2,
                           A.Offset, A.Expression, A.OperandBytes, A.RAState) ==
                  std::tie(B.Kind, B.CodeOffset, B.Register, B.Register2,
                           B.Offset, B.Expression, B.OperandBytes, B.RAState);
         });
}

llvm::Error compareFrameStates(const dwarf_eh::ParseResult &Before,
                               const dwarf_eh::ParseResult &After,
                               Arch Architecture, uint64_t RemainingWork) {
  if (Before.ParseStatus != ExceptionParseStatus::Complete ||
      After.ParseStatus != ExceptionParseStatus::Complete ||
      Before.CIEs.size() != After.CIEs.size() ||
      Before.FDEs.size() != After.FDEs.size())
    return fail(kFrameIdentityChanged);
  const auto CIEIdentity = [](const DwarfCIE &C) {
    return std::tie(
        C.SectionOffset, C.Version, C.Augmentation, C.CodeAlignmentFactor,
        C.DataAlignmentFactor, C.ReturnAddressRegister, C.AddressSize,
        C.SegmentSelectorSize, C.FDEPointerEncoding, C.LSDAPointerEncoding,
        C.PersonalityEncoding, C.PersonalityVA, C.PersonalitySlotVA,
        C.HasAugmentationData, C.IsSignalFrame, C.HasPointerAuth,
        C.UsesArm64eReturnAddressABI, C.UsesNonHintPAuthLR, C.HasMTETaggedFrame,
        C.ParseStatus);
  };
  std::map<uint64_t, size_t> CIEIndices;
  for (size_t Index = 0; Index < Before.CIEs.size(); ++Index) {
    const auto &Old = Before.CIEs[Index];
    const auto &New = After.CIEs[Index];
    if (CIEIdentity(Old) != CIEIdentity(New) ||
        !sameCFI(Old.InitialInstructions, New.InitialInstructions) ||
        !CIEIndices.emplace(Old.SectionOffset, Index).second)
      return fail(kFrameIdentityChanged);
  }
  const auto FDEIdentity = [](const DwarfFDE &F) {
    return std::tie(F.SectionOffset, F.CIESectionOffset, F.InitialLocation,
                    F.InitialLocationOffset, F.AddressRange, F.LSDAVA,
                    F.ParseStatus);
  };
  for (size_t Index = 0; Index < Before.FDEs.size(); ++Index) {
    const auto &Old = Before.FDEs[Index];
    const auto &New = After.FDEs[Index];
    const auto Found = CIEIndices.find(Old.CIESectionOffset);
    if (Found == CIEIndices.end() || FDEIdentity(Old) != FDEIdentity(New) ||
        !sameCFI(Old.Instructions, New.Instructions))
      return fail(kFrameIdentityChanged);
    if (Architecture != Arch::AArch64)
      continue;
    const auto &OldCIE = Before.CIEs[Found->second];
    const auto &NewCIE = After.CIEs[Found->second];
    const uint64_t InitialCount = OldCIE.InitialInstructions.size();
    const uint64_t Count = Old.Instructions.size();
    if (Count >= UINT64_MAX - InitialCount)
      return fail(kFrameStateBudget);
    const uint64_t Units = InitialCount + Count + 1;
    if (Units > RemainingWork / kRAComparisonPasses)
      return fail(kFrameStateBudget);
    const uint64_t PerBoundary = Units * kRAComparisonPasses;
    const uint64_t BoundaryLimit = RemainingWork / PerBoundary;
    std::set<uint64_t> Boundaries{0};
    const auto AddBoundary = [&](uint64_t Offset) {
      if (Offset < Old.AddressRange)
        Boundaries.insert(Offset);
      return Boundaries.size() <= BoundaryLimit;
    };
    for (const auto &I : Old.Instructions) {
      if (I.CodeOffset > Old.AddressRange)
        return fail(kFrameIdentityChanged);
      if (!AddBoundary(I.CodeOffset))
        return fail(kFrameStateBudget);
      if (I.Kind != CFIOpKind::AdvanceLoc)
        continue;
      if (I.Offset < 0 || !OldCIE.CodeAlignmentFactor ||
          uint64_t(I.Offset) >
              (UINT64_MAX - I.CodeOffset) / OldCIE.CodeAlignmentFactor)
        return fail(kFrameIdentityChanged);
      const uint64_t Next =
          I.CodeOffset + uint64_t(I.Offset) * OldCIE.CodeAlignmentFactor;
      if (Next > Old.AddressRange)
        return fail(kFrameIdentityChanged);
      if (!AddBoundary(Next))
        return fail(kFrameStateBudget);
    }
    RemainingWork -= Boundaries.size() * PerBoundary;
    for (uint64_t Offset : Boundaries) {
      if (!Old.AddressRange || Offset >= Old.AddressRange ||
          Old.InitialLocation > UINT64_MAX - Offset)
        return fail(kFrameIdentityChanged);
      const auto A = dwarf_eh::evaluateAArch64ReturnAddressState(
          Architecture, OldCIE, Old, Old.InitialLocation + Offset);
      const auto B = dwarf_eh::evaluateAArch64ReturnAddressState(
          Architecture, NewCIE, New, New.InitialLocation + Offset);
      if (A.Status != dwarf_eh::AArch64RAQueryStatus::Known ||
          B.Status != dwarf_eh::AArch64RAQueryStatus::Known || !A.State ||
          !B.State)
        return fail(kFrameStateUnavailable);
      if (*A.State != *B.State)
        return fail(kFrameStateChanged);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>>
relocatedFrames(const std::vector<uint8_t> &Binary, const MachOEHFrameRegion &R,
                uint64_t VA, const MachOUnwindRelocationLimits &Limits) {
  const auto Raw = llvm::ArrayRef<uint8_t>(Binary).slice(
      R.SectionFileOff, R.AppendFileOff - R.SectionFileOff);
  auto Records = decodeDwarfEHFrameRecords(Raw, R.SectionVA, R.Is64);
  if (!Records)
    return Records.takeError();
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Image = Reader->loadBuffer(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Binary.data()),
                      Binary.size()),
      "layout-frames"));
  if (!Image)
    return Image.takeError();
  const auto OriginalFrames =
      dwarf_eh::parseEHFrame(*Image, {Raw.data(), Raw.size(), R.SectionVA}, {});
  auto StableRules = [](llvm::ArrayRef<CFIInstruction> Rules) {
    return llvm::none_of(Rules, [](const auto &I) {
      return I.Kind == CFIOpKind::SetLoc || I.Kind == CFIOpKind::Opaque ||
             I.Kind == CFIOpKind::Expression ||
             I.Kind == CFIOpKind::ValExpression ||
             I.Kind == CFIOpKind::DefCFAExpression;
    });
  };
  for (const auto &S : Image->Symbols)
    if (S.Addr >= R.SectionVA && S.Addr - R.SectionVA < Raw.size())
      return fail("named EH storage needs symbol/reference relocation");
  std::vector<uint8_t> Result(Raw.begin(), Raw.end());
  for (const auto &Record : *Records) {
    const auto *F = Image->ExceptionMetadata.findFunction(Record.BeginVA);
    const auto *C =
        F && F->Dwarf
            ? Image->ExceptionMetadata.findCIE(F->Dwarf->CIESectionOffset)
            : nullptr;
    if (!F || !C || F->ParseStatus != ExceptionParseStatus::Complete ||
        F->Dwarf->InitialLocation != Record.BeginVA ||
        F->Dwarf->AddressRange != Record.EndVA - Record.BeginVA ||
        !hasRelocatableAugmentation(*C, Image->Arch) || C->IsSignalFrame ||
        C->HasMTETaggedFrame || C->PersonalityVA || C->PersonalitySlotVA ||
        F->Dwarf->LSDAVA || !StableRules(C->InitialInstructions) ||
        !StableRules(F->Dwarf->Instructions) ||
        Record.HasLanguageAugmentation ||
        (Record.InitialLocationEncoding != 0x10 &&
         Record.InitialLocationEncoding != 0x1b &&
         Record.InitialLocationEncoding != 0x1c) ||
        !rangeInBounds(Record.InitialLocationOffset,
                       Record.InitialLocationWidth, Result.size()))
      return fail(
          "EH growth needs an address-stable, non-language DWARF contract: "
          "VA=" +
          llvm::utohexstr(Record.BeginVA) + ", CIE=" +
          (C ? C->Augmentation : std::string("absent")) + ", encoding=" +
          llvm::Twine(unsigned(Record.InitialLocationEncoding)) +
          ", status=" + llvm::Twine(F ? unsigned(F->ParseStatus) : 255));
    const int64_t Delta =
        int64_t(Record.BeginVA) - int64_t(VA + Record.InitialLocationOffset);
    if (Record.InitialLocationWidth == 4 && llvm::isInt<32>(Delta))
      writeLE<uint32_t>(Result.data() + Record.InitialLocationOffset,
                        uint32_t(Delta));
    else if (Record.InitialLocationWidth == 8)
      writeLE<uint64_t>(Result.data() + Record.InitialLocationOffset,
                        uint64_t(Delta));
    else
      return fail("relocated FDE exceeds its encoded reach");
  }
  auto Replay = decodeDwarfEHFrameRecords(Result, VA, R.Is64);
  if (!Replay)
    return Replay.takeError();
  if (Replay->size() != Records->size())
    return fail("relocated FDE count changed");
  for (size_t I = 0; I < Replay->size(); ++I)
    if ((*Replay)[I].BeginVA != (*Records)[I].BeginVA ||
        (*Replay)[I].EndVA != (*Records)[I].EndVA)
      return fail("relocated FDE changed its original code range");
  const auto RelocatedFrames =
      dwarf_eh::parseEHFrame(*Image, {Result.data(), Result.size(), VA}, {});
  if (auto Error = compareFrameStates(OriginalFrames, RelocatedFrames,
                                      Image->Arch, Limits.MaxRAStateWork))
    return Error;
  return Result;
}
} // namespace

llvm::Expected<uint64_t> declareMachOTextMetadata(std::vector<uint8_t> &Binary,
                                                  Arch A, llvm::StringRef Name,
                                                  uint64_t Size) {
  using namespace llvm::MachO;
  if (Name.empty() || Name.size() > 16 || !Size ||
      Size > ::neverd::limits::backend::
                 kMachOLayoutGrowthDeclareMachOTextMetadataSizeLimit)
    return fail("invalid metadata section request");
  auto B = Binary;
  auto Header = reserveMachOHeaderStorage(
      B, A, (A == Arch::X64 || A == Arch::AArch64) ? 512 + Size : 512);
  if (!Header)
    return Header.takeError();
  std::vector<macho_patch_detail::MachOFileRange> Segments, Sections;
  if (!macho_patch_detail::collectMachOFileRanges(B, Segments, Sections))
    return fail("invalid original file layout");
  const auto H = parseMachOHeader(B.data(), B.size());
  const uint64_t End = uint64_t(H.HeaderSize) + H.SizeOfCmds;
  const unsigned SS = getMachOSectionSize(H.Is64);
  uint64_t First = B.size(), Text = UINT64_MAX, Insert = 0;
  uint64_t Last = 0, TextVA = 0, TextOff = 0, TextSize = 0;
  uint32_t TextCS = 0, Ordinal = 0, NewOrdinal = 0;
  bool Invalid = false;
  forEachMachOLoadCommand(
      B.data(), B.size(),
      [&](const uint8_t *C, uint32_t Cmd, uint32_t CS, bool Is64) {
        if (Cmd == LC_DYSYMTAB) {
          const auto *D = reinterpret_cast<const dysymtab_command *>(C);
          Invalid |= D->nextrel || D->nlocrel;
        }
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;
        const auto F = readMachOSegment(C, Is64);
        const bool IsText = readMachOName(F.SegName) == "__TEXT";
        if (CS != getMachOSegmentCmdSize(Is64) + uint64_t(F.NSects) * SS) {
          Invalid = true;
          return;
        }
        const uint8_t *P = C + getMachOSegmentCmdSize(Is64);
        for (unsigned I = 0; I < F.NSects; ++I, P += SS) {
          ++Ordinal;
          uint64_t Off, Extent;
          uint32_t Relocations;
          const char *SN;
          if (Is64) {
            const auto *S = reinterpret_cast<const section_64 *>(P);
            Off = S->offset;
            Extent = S->size;
            Relocations = S->nreloc;
            SN = S->sectname;
          } else {
            const auto *S = reinterpret_cast<const section *>(P);
            Off = S->offset;
            Extent = S->size;
            Relocations = S->nreloc;
            SN = S->sectname;
          }
          Invalid |= Relocations != 0;
          if (Off && Extent)
            First = std::min(First, Off);
          if (IsText && Off && Extent)
            Last = std::max(Last, Off + Extent);
          if (IsText && readMachOName(SN) == Name)
            Invalid = true;
        }
        if (!IsText)
          return;
        if (Text != UINT64_MAX || F.InitProt != 5 || F.MaxProt != 5) {
          Invalid = true;
          return;
        }
        Text = C - B.data();
        Insert = Text + CS;
        TextCS = CS;
        TextVA = F.VMAddr;
        TextOff = F.FileOff;
        TextSize = F.FileSize;
        NewOrdinal = Ordinal + 1;
      });
  if (Invalid || Text == UINT64_MAX || Ordinal >= 255 || End + SS > First ||
      !rangeInBounds(End, SS, B.size()) ||
      llvm::any_of(llvm::ArrayRef<uint8_t>(B).slice(End, SS),
                   [](uint8_t V) { return V != 0; }))
    return fail("new TEXT section needs authenticated header capacity and "
                "section identities");
  // reserveMachOHeaderStorage authenticates this entire prefix as unowned.
  const uint64_t Prefix = llvm::alignTo(End + 512, 8);
  const bool PrefixFits =
      Prefix <= First && Size <= First - Prefix &&
      rangeInBounds(Prefix, Size, B.size()) &&
      llvm::all_of(llvm::ArrayRef<uint8_t>(B).slice(Prefix, Size),
                   [](uint8_t V) { return !V; });
  uint64_t Off =
      PrefixFits ? Prefix : llvm::alignTo(std::max(Last, End + SS), 8);
  uint64_t VA = TextVA + Off - TextOff;
  if (Off < TextOff || !rangeInBounds(Off - TextOff, Size, TextSize) ||
      !rangeInBounds(Off, Size, B.size()) ||
      llvm::any_of(
          llvm::ArrayRef<uint8_t>(B).slice(Off, Size),
          [](uint8_t V) { return V != 0; })) {
    MachOPatcher P;
    P.setInputKind(RewriteInputKind::ReplacementIR);
    std::vector<uint8_t> Zeros(llvm::alignTo(Size, 4096), 0);
    VA = P.appendExecSegment(B, Zeros, "__TEXT", A);
    if (!VA || VA < TextVA)
      return fail("new metadata cannot extend the original TEXT segment");
    Off = TextOff + VA - TextVA;
    const auto F = readMachOSegment(B.data() + Text, H.Is64);
    if (!rangeInBounds(VA - TextVA, Size, F.FileSize))
      return fail("new metadata is outside TEXT");
  }
  if (Off > UINT32_MAX || (!H.Is64 && VA > UINT32_MAX))
    return fail("new metadata exceeds Mach-O address limits");
  // Inserting a section changes n_sect for all following sections. The dyld
  // segment ordinals remain stable; native relocatable images are rejected
  // above.
  forEachMachOLoadCommand(
      B.data(), B.size(),
      [&](const uint8_t *C, uint32_t Cmd, uint32_t, bool Is64) {
        if (Cmd != LC_SYMTAB)
          return;
        const auto *T = reinterpret_cast<const symtab_command *>(C);
        const unsigned NS = Is64 ? sizeof(nlist_64) : sizeof(nlist);
        if (!rangeInBounds(T->symoff, uint64_t(T->nsyms) * NS, B.size())) {
          Invalid = true;
          return;
        }
        for (unsigned I = 0; I < T->nsyms; ++I) {
          uint8_t *N = B.data() + T->symoff + uint64_t(I) * NS;
          if ((N[4] & N_STAB) && N[5]) {
            Invalid = true;
            return;
          }
          if (N[5] > Ordinal) {
            Invalid = true;
            return;
          }
          if (N[5] >= NewOrdinal)
            ++N[5];
        }
      });
  if (Invalid)
    return fail("section insertion has unmodeled symbol ordinals");
  const auto UpdatedH = parseMachOHeader(B.data(), B.size());
  const uint64_t UpdatedEnd =
      uint64_t(UpdatedH.HeaderSize) + UpdatedH.SizeOfCmds;
  std::memmove(B.data() + Insert + SS, B.data() + Insert, UpdatedEnd - Insert);
  std::memset(B.data() + Insert, 0, SS);
  if (H.Is64) {
    auto *G = reinterpret_cast<segment_command_64 *>(B.data() + Text);
    G->cmdsize += SS;
    ++G->nsects;
    auto *S = reinterpret_cast<section_64 *>(B.data() + Insert);
    std::memcpy(S->sectname, Name.data(), Name.size());
    std::memcpy(S->segname, "__TEXT", 6);
    S->addr = VA;
    S->offset = Off;
    S->size = Size;
    S->align = 3;
    reinterpret_cast<mach_header_64 *>(B.data())->sizeofcmds += SS;
  } else {
    auto *G = reinterpret_cast<segment_command *>(B.data() + Text);
    G->cmdsize += SS;
    ++G->nsects;
    auto *S = reinterpret_cast<section *>(B.data() + Insert);
    std::memcpy(S->sectname, Name.data(), Name.size());
    std::memcpy(S->segname, "__TEXT", 6);
    S->addr = VA;
    S->offset = Off;
    S->size = Size;
    S->align = 3;
    reinterpret_cast<mach_header *>(B.data())->sizeofcmds += SS;
  }
  Segments.clear();
  Sections.clear();
  if (!macho_patch_detail::collectMachOFileRanges(B, Segments, Sections))
    return fail("new section changed file ownership");
  Binary.swap(B);
  return VA;
}

llvm::Expected<bool> reserveMachOSourceUnwind(
    std::vector<uint8_t> &Binary, Arch A, uint64_t AdditionalEHBytes,
    uint64_t AdditionalFunctions, const MachOUnwindRelocationLimits &Limits) {
  if (AdditionalEHBytes >
          ::neverd::limits::backend::
              kMachOLayoutGrowthReserveMachOSourceUnwindAdditionalEHBytesLimit ||
      AdditionalFunctions >
          ::neverd::limits::backend::kMaxMachOAdditionalUnwindFunctions)
    return fail("unwind request exceeds supported bounds");
  auto Candidate = Binary;
  auto Compact = findMachOCompactUnwindRegion(Candidate);
  if (!Compact)
    return Compact.takeError();
  if (!*Compact && AdditionalFunctions) {
    // Create the registry before adding RX/RW segments. Existing DWARF must
    // remain reachable: an encoding-zero gap would disable libunwind fallback.
    std::vector<NativeMachOCompactRange> OriginalRanges;
    if (auto EH = findMachOEHFrameRegion(Candidate)) {
      auto Records = decodeDwarfEHFrameRecords(
          llvm::ArrayRef<uint8_t>(Candidate).slice(
              EH->SectionFileOff, EH->AppendFileOff - EH->SectionFileOff),
          EH->SectionVA, EH->Is64);
      if (!Records)
        return Records.takeError();
      for (const auto &R : *Records) {
        const auto Offset = R.RecordVA - EH->SectionVA;
        if (Offset > 0xffffff)
          return fail("original FDE exceeds compact encoding");
        OriginalRanges.push_back(
            {R.BeginVA, R.EndVA,
             (A == Arch::AArch64 ? 0x03000000u : 0x04000000u) |
                 uint32_t(Offset)});
      }
    }
    if (OriginalRanges.empty()) {
      // Explicit absence preserves legacy SJLJ/no-unwind input semantics.
      forEachMachOLoadCommand(
          Candidate.data(), Candidate.size(),
          [&](const uint8_t *C, uint32_t Cmd, uint32_t, bool Is64) {
            if (Cmd != getMachOSegmentCmdID(Is64))
              return;
            const auto G = readMachOSegment(C, Is64);
            auto *P = C + getMachOSegmentCmdSize(Is64);
            for (unsigned I = 0; I < G.NSects;
                 ++I, P += getMachOSectionSize(Is64)) {
              if (readMachOName(reinterpret_cast<const char *>(P)) != "__text")
                continue;
              uint64_t VA, Size;
              if (Is64) {
                auto *S = reinterpret_cast<const llvm::MachO::section_64 *>(P);
                VA = S->addr;
                Size = S->size;
              } else {
                auto *S = reinterpret_cast<const llvm::MachO::section *>(P);
                VA = S->addr;
                Size = S->size;
              }
              if (Size)
                OriginalRanges.push_back({VA, VA + Size, 0});
            }
          });
    }
    if (OriginalRanges.empty())
      return fail("cannot preserve original unwind coverage");
    if (auto E = installNativeMachOCompactUnwind(Candidate, A, OriginalRanges))
      return E;
    Compact = findMachOCompactUnwindRegion(Candidate);
    if (!Compact)
      return Compact.takeError();
  }
  if (*Compact && AdditionalFunctions) {
    const auto R = **Compact;
    auto Raw = macho_unwind::parseCompactUnwindRaw(
        llvm::ArrayRef<uint8_t>(Candidate).slice(R.SectionFileOff,
                                                 R.SectionSize));
    if (!Raw)
      return Raw.takeError();
    uint64_t Count = 0;
    for (const auto &Page : Raw->Pages)
      Count += Page.EntryCount;
    // Bound regular encoding, including gap records and page indexes.
    // Regular pages occupy 4096 bytes even when only partially filled.
    const uint64_t Required =
        4096 + 256 + (Count + 3 * AdditionalFunctions + 1) * 32 +
        Raw->LSDAEntries.size() * 8 + Raw->PersonalitySlotOffsets.size() * 4;
    if (Required > R.SectionSize) {
      if (auto E = reserveMachOCompactUnwindStorage(Candidate, Required)) {
        llvm::consumeError(std::move(E));
        const uint64_t Capacity = llvm::alignTo(Required, 4096);
        // Adopt checked prefix growth before placing the compact registry.
        // The growth owner rebases its TEXT-relative records, so refresh the
        // payload instead of copying the pre-growth offsets into the new map.
        auto Header = reserveMachOHeaderStorage(Candidate, A, 512 + Capacity);
        if (!Header)
          llvm::consumeError(Header.takeError());
        else if (*Header) {
          auto Updated = findMachOCompactUnwindRegion(Candidate);
          if (!Updated)
            return Updated.takeError();
          if (!*Updated)
            return fail("header growth lost the compact registry");
          Raw = macho_unwind::parseCompactUnwindRaw(
              llvm::ArrayRef<uint8_t>(Candidate).slice(
                  (**Updated).SectionFileOff, (**Updated).SectionSize));
          if (!Raw)
            return Raw.takeError();
        }
        auto Moved = moveTextSection(Candidate, A, "__unwind_info",
                                     Raw->OriginalBytes, Capacity);
        if (!Moved)
          return Moved.takeError();
      }
    }
  }
  if (AdditionalEHBytes) {
    auto R = findMachOEHFrameRegion(Candidate);
    if (!R) {
      auto Declared =
          declareMachOTextMetadata(Candidate, A, "__eh_frame",
                                   llvm::alignTo(AdditionalEHBytes + 4, 4096));
      if (!Declared)
        return Declared.takeError();
      R = findMachOEHFrameRegion(Candidate);
      if (!R)
        return fail("new DWARF section cannot be rediscovered");
    }
    const bool HasCapacity =
        AdditionalEHBytes <= R->LimitFileOff - R->AppendFileOff &&
        llvm::all_of(llvm::ArrayRef<uint8_t>(Candidate).slice(
                         R->AppendFileOff, AdditionalEHBytes),
                     [](uint8_t B) { return B == 0; });
    if (!HasCapacity) {
      MachOPatcher P;
      P.setInputKind(RewriteInputKind::ReplacementIR);
      const uint64_t VA = P.plannedExecSegmentVA(Candidate, A);
      if (!VA)
        return fail("cannot plan relocated frame storage");
      auto Frames = relocatedFrames(Candidate, *R, VA, Limits);
      if (!Frames)
        return Frames.takeError();
      auto Moved = moveTextSection(
          Candidate, A, "__eh_frame", *Frames,
          llvm::alignTo(Frames->size() + AdditionalEHBytes + 4, 4096));
      if (!Moved)
        return Moved.takeError();
      if (*Moved != VA)
        return fail("relocated frame placement changed");
    }
  }
  const bool Changed = Candidate != Binary;
  Binary.swap(Candidate);
  return Changed;
}

llvm::Expected<bool>
reserveMachONativePatchStorage(std::vector<uint8_t> &Binary, Arch A,
                               llvm::StringRef Object, uint64_t CallbackCount,
                               uint64_t ImportFunctionCount) {
  auto File = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(Object, "layout-object"));
  if (!File)
    return File.takeError();
  if (!llvm::isa<llvm::object::MachOObjectFile>(File->get()))
    return fail("layout producer is not Mach-O");
  uint64_t Frames = 0, Symbols = 0;
  for (const auto &S : (*File)->sections()) {
    auto Name = S.getName();
    if (!Name)
      return Name.takeError();
    if (*Name == "__eh_frame")
      Frames += S.getSize();
  }
  for (const auto &S : (*File)->symbols()) {
    (void)S;
    ++Symbols;
  }
  if (Frames > ::neverd::limits::backend::kMaxMachONativePatchFrames ||
      CallbackCount >
          ::neverd::limits::backend::
              kMachOLayoutGrowthReserveMachONativePatchStorageCallbackCountLimit ||
      ImportFunctionCount >
          ::neverd::limits::backend::
              kMachOLayoutGrowthReserveMachONativePatchStorageImportFunctionCountLimit)
    return fail("layout request exceeds native metadata limits");
  return reserveMachOSourceUnwind(
      Binary, A,
      llvm::alignTo(Frames, 8) + CallbackCount * 1024 +
          ImportFunctionCount * 64,
      Symbols + CallbackCount + ImportFunctionCount);
}
} // namespace neverd
