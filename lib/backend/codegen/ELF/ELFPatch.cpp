//===- ELFPatch.cpp - ELF binary patching -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF binary patching implementation: appends a new PT_LOAD segment
/// containing recompiled code and injects trampolines at original function
/// entry points.  Handles both ELF32 and ELF64 in a unified code path.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFPatch.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/ELF/ELFARMEHABIPatch.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/backend/codegen/ELF/ELFSectionPatch.h"
#include "neverd/backend/codegen/ELF/ELFSourceRelocations.h"
#include "neverd/backend/codegen/ELF/ELFSourceSymbols.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>

#define DEBUG_TYPE "neverd-elf-patch"

namespace neverd {
namespace {

std::optional<uint64_t> checkedAdd(uint64_t Left, uint64_t Right) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return std::nullopt;
  return Left + Right;
}

std::optional<uint64_t> checkedAlignUp(uint64_t Value, uint64_t Alignment) {
  if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
    return std::nullopt;
  const uint64_t Mask = Alignment - 1;
  if (Value > std::numeric_limits<uint64_t>::max() - Mask)
    return std::nullopt;
  return (Value + Mask) & ~Mask;
}

std::optional<uint32_t> checkedELF32Field(uint64_t Value) {
  if (Value > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  return static_cast<uint32_t>(Value);
}

bool isValidELF32Range(uint64_t Start, uint64_t Size) {
  constexpr uint64_t AddressSpaceEnd =
      uint64_t{std::numeric_limits<uint32_t>::max()} + 1;
  const std::optional<uint64_t> End = checkedAdd(Start, Size);
  return checkedELF32Field(Start).has_value() &&
         checkedELF32Field(Size).has_value() && End && *End <= AddressSpaceEnd;
}

bool rangesOverlap(llvm::ArrayRef<uint8_t> Left,
                   llvm::ArrayRef<uint8_t> Right) {
  if (Left.empty() || Right.empty())
    return false;
  std::less<const uint8_t *> IsBefore;
  return IsBefore(Left.data(), Right.end()) &&
         IsBefore(Right.data(), Left.end());
}

struct ExecSegmentPlan {
  uint16_t NewPhNum = 0;
  uint64_t PhdrTableSize = 0;
  uint64_t SegmentFileOffset = 0;
  uint64_t SegmentVA = 0;
  uint64_t CodeFileOffset = 0;
  uint64_t CodeVA = 0;
  uint64_t FileSize = 0;
  uint64_t MemorySize = 0;
  uint64_t OutputSize = 0;
};

std::optional<ExecSegmentPlan>
planExecSegment(bool Is64, uint64_t MaxVA, uint64_t BinarySize,
                uint16_t OldPhNum, uint16_t PhdrSize, uint64_t PageSize,
                uint64_t CodeSize, bool SplitStorage) {
  const uint16_t AddedHeaders = SplitStorage ? 2 : 1;
  if (OldPhNum >= llvm::ELF::PN_XNUM - AddedHeaders)
    return std::nullopt;

  ExecSegmentPlan Plan;
  Plan.NewPhNum = OldPhNum + AddedHeaders;
  Plan.PhdrTableSize = static_cast<uint64_t>(Plan.NewPhNum) * PhdrSize;
  const uint64_t PrefixSize =
      SplitStorage ? llvm::alignTo(Plan.PhdrTableSize, 16) : Plan.PhdrTableSize;

  const std::optional<uint64_t> SegmentFileOffset =
      checkedAlignUp(BinarySize, PageSize);
  const std::optional<uint64_t> SegmentVA = checkedAlignUp(MaxVA, PageSize);
  if (!SegmentFileOffset || !SegmentVA)
    return std::nullopt;
  Plan.SegmentFileOffset = *SegmentFileOffset;
  Plan.SegmentVA = *SegmentVA;

  const std::optional<uint64_t> CodeFileOffset =
      checkedAdd(Plan.SegmentFileOffset, PrefixSize);
  const std::optional<uint64_t> CodeVA = checkedAdd(Plan.SegmentVA, PrefixSize);
  const std::optional<uint64_t> FileSize = checkedAdd(PrefixSize, CodeSize);
  const std::optional<uint64_t> MemorySize =
      FileSize ? checkedAlignUp(*FileSize, PageSize) : std::nullopt;
  const std::optional<uint64_t> OutputSize =
      MemorySize ? checkedAdd(Plan.SegmentFileOffset, *MemorySize)
                 : std::nullopt;
  if (!CodeFileOffset || !CodeVA || !FileSize || !MemorySize || !OutputSize ||
      *OutputSize > std::numeric_limits<size_t>::max())
    return std::nullopt;

  Plan.CodeFileOffset = *CodeFileOffset;
  Plan.CodeVA = *CodeVA;
  Plan.FileSize = *FileSize;
  Plan.MemorySize = *MemorySize;
  Plan.OutputSize = *OutputSize;

  if (!Is64) {
    constexpr uint64_t AddressSpaceEnd =
        uint64_t{std::numeric_limits<uint32_t>::max()} + 1;
    const bool FieldsFit =
        checkedELF32Field(Plan.SegmentFileOffset).has_value() &&
        checkedELF32Field(Plan.SegmentVA).has_value() &&
        checkedELF32Field(Plan.CodeFileOffset).has_value() &&
        checkedELF32Field(Plan.CodeVA).has_value() &&
        checkedELF32Field(Plan.PhdrTableSize).has_value() &&
        checkedELF32Field(Plan.FileSize).has_value() &&
        checkedELF32Field(Plan.MemorySize).has_value() &&
        checkedELF32Field(PageSize).has_value();
    if (!FieldsFit ||
        !isValidELF32Range(Plan.SegmentFileOffset, Plan.FileSize) ||
        !isValidELF32Range(Plan.SegmentVA, Plan.MemorySize) ||
        Plan.OutputSize > AddressSpaceEnd)
      return std::nullopt;
  }

  return Plan;
}

llvm::Expected<std::vector<TextLayout>> collectSourceTrampolineRegions(
    llvm::ArrayRef<uint8_t> Binary, const BinaryImage &Image,
    const std::map<std::string, uint64_t> &OriginalVAs) {
  auto Reject = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "elf source entry: " + Detail);
  };
  std::vector<ELFPhdrFields> Loads;
  forEachELFPhdr(Binary.data(), Binary.size(),
                 [&](const ELFPhdrFields &F, const uint8_t *, bool) {
                   if (F.Type == llvm::ELF::PT_LOAD)
                     Loads.push_back(F);
                 });
  std::vector<TextLayout> Regions;
  for (const auto &[Name, Entry] : OriginalVAs) {
    const auto Mode = Image.instructionModeAt(Entry);
    const uint64_t Width =
        Mode ? getTargetCodegenInfo(Image.Arch, *Mode).trampolineSize() : 0;
    if (!Width || !Image.hasAuthenticatedFunctionEntryAt(Entry) ||
        Entry > UINT64_MAX - Width)
      return Reject("source entry has no checked instruction-mode contract");
    const ELFPhdrFields *Owner = nullptr;
    for (const auto &Load : Loads) {
      if (Load.MemSz && Load.VAddr > Entry && Load.VAddr - Entry < Width)
        return Reject("source entry crosses another load segment's mapping");
      if (Entry >= Load.VAddr && Entry - Load.VAddr < Load.MemSz) {
        if (Owner || !(Load.Flags & llvm::ELF::PF_X) ||
            Entry - Load.VAddr > Load.FileSz ||
            Width > Load.FileSz - (Entry - Load.VAddr))
          return Reject("source entry has ambiguous or non-executable storage");
        Owner = &Load;
      }
    }
    if (!Owner)
      return Reject("source entry has no file-backed executable owner");
    const uint64_t Delta = Entry - Owner->VAddr;
    if (Delta > UINT64_MAX - Owner->Offset ||
        !rangeInBounds(Owner->Offset + Delta, Width, Binary.size()))
      return Reject("source entry file range is invalid");
    const uint64_t Offset = Owner->Offset + Delta;
    for (const auto &Load : Loads)
      if (&Load != Owner && Load.FileSz &&
          (Offset >= Load.Offset ? Offset - Load.Offset < Load.FileSz
                                 : Load.Offset - Offset < Width))
        return Reject("source entry aliases another load segment's storage");
    const uint8_t *Source = Image.readVA(Entry, Width);
    if (!Source || !std::equal(Source, Source + Width, Binary.begin() + Offset))
      return Reject("source entry bytes changed after analysis");
    Regions.push_back({Offset, Entry, Width});
  }
  return Regions;
}

bool hasValidELFIdentity(llvm::ArrayRef<uint8_t> Data) {
  return Data.size() >= llvm::ELF::EI_NIDENT &&
         (Data[llvm::ELF::EI_CLASS] == llvm::ELF::ELFCLASS32 ||
          Data[llvm::ELF::EI_CLASS] == llvm::ELF::ELFCLASS64) &&
         Data[llvm::ELF::EI_DATA] == llvm::ELF::ELFDATA2LSB &&
         Data[llvm::ELF::EI_VERSION] == llvm::ELF::EV_CURRENT;
}

} // namespace

bool ELFPatcher::parseLayout(const std::vector<uint8_t> &Data,
                             PatchLayout &Layout) {
  if (!hasValidELFIdentity(Data)) {
    llvm::WithColor::error() << "elf_patch: not a valid ELF file\n";
    return false;
  }
  auto Hdr = parseELFHeader(Data.data(), Data.size());
  const uint64_t EHdrSize = Hdr.Is64 ? sizeof(llvm::object::ELF64LE::Ehdr)
                                     : sizeof(llvm::object::ELF32LE::Ehdr);
  if (Data.size() < EHdrSize) {
    llvm::WithColor::error() << "elf_patch: truncated ELF header\n";
    return false;
  }
  const uint64_t PhdrSize = getELFPhdrSize(Hdr.Is64);
  const uint64_t PhdrTableSize = static_cast<uint64_t>(Hdr.PhNum) * PhdrSize;
  const uint16_t DeclaredEHdrSize =
      Hdr.Is64 ? static_cast<uint16_t>(
                     reinterpret_cast<const llvm::object::ELF64LE::Ehdr *>(
                         Data.data())
                         ->e_ehsize)
               : static_cast<uint16_t>(
                     reinterpret_cast<const llvm::object::ELF32LE::Ehdr *>(
                         Data.data())
                         ->e_ehsize);
  if (Hdr.HeaderSize != EHdrSize || DeclaredEHdrSize != EHdrSize ||
      Hdr.PhNum == 0 || Hdr.PhEntSize != PhdrSize ||
      !rangeInBounds(Hdr.PhOff, PhdrTableSize, Data.size()) ||
      (!Hdr.Is64 && !isValidELF32Range(Hdr.PhOff, PhdrTableSize))) {
    llvm::WithColor::error() << "elf_patch: not a valid ELF file\n";
    return false;
  }

  PatchLayout Candidate;
  Candidate.Is64 = Hdr.Is64;
  Candidate.EntryPoint = Hdr.EntryPoint;
  Candidate.PhNum = Hdr.PhNum;
  Candidate.PhOff = Hdr.PhOff;
  bool IsValid = true;
  bool SawLoad = false;
  bool SawExecutableLoad = false;

  forEachELFPhdr(
      Data.data(), Data.size(),
      [&](const ELFPhdrFields &F, const uint8_t *, bool) {
        if (F.Type != llvm::ELF::PT_LOAD)
          return;

        SawLoad = true;
        const std::optional<uint64_t> EndVA = checkedAdd(F.VAddr, F.MemSz);
        const std::optional<uint64_t> EndFile = checkedAdd(F.Offset, F.FileSz);
        const bool AlignmentValid = F.Align == 0 || F.Align == 1 ||
                                    (((F.Align & (F.Align - 1)) == 0) &&
                                     F.VAddr % F.Align == F.Offset % F.Align);
        const bool AddressWidthValid =
            Candidate.Is64 || (isValidELF32Range(F.VAddr, F.MemSz) &&
                               isValidELF32Range(F.Offset, F.FileSz));
        if (!EndVA || !EndFile || F.FileSz > F.MemSz ||
            !rangeInBounds(F.Offset, F.FileSz, Data.size()) ||
            !AlignmentValid || !AddressWidthValid) {
          IsValid = false;
          return;
        }
        Candidate.MaxVA = std::max(Candidate.MaxVA, *EndVA);
        Candidate.MaxFileOff = std::max(Candidate.MaxFileOff, *EndFile);

        if (F.Flags & llvm::ELF::PF_X) {
          SawExecutableLoad = true;
          Candidate.TextVA = F.VAddr;
          Candidate.TextSize = F.MemSz;
          Candidate.TextFileOff = F.Offset;
          Candidate.TextFileSize = F.FileSz;
        }
      });

  if (!IsValid || !SawLoad || !SawExecutableLoad) {
    llvm::WithColor::error() << "elf_patch: malformed load segments\n";
    return false;
  }
  Layout = Candidate;

  LLVM_DEBUG(llvm::dbgs() << "elf_patch: " << (Layout.Is64 ? "ELF64" : "ELF32")
                          << " entry=0x" << llvm::utohexstr(Layout.EntryPoint)
                          << " text VA=0x" << llvm::utohexstr(Layout.TextVA)
                          << " size=0x" << llvm::utohexstr(Layout.TextSize)
                          << "\n");
  return true;
}

uint64_t ELFPatcher::plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                          Arch TargetArch) {
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;
  // A copy of the (grown-by-one) phdr table is prepended to the new segment,
  // so the code lives just past it.
  auto EHdr = parseELFHeader(Binary.data(), Binary.size());
  const uint64_t PageSize = elfPageSize(TargetArch);
  const std::optional<ExecSegmentPlan> Plan =
      planExecSegment(Layout.Is64, Layout.MaxVA, Binary.size(), EHdr.PhNum,
                      getELFPhdrSize(Layout.Is64), PageSize, /*CodeSize=*/0,
                      InputKind != RewriteInputKind::LiftedIR);
  return Plan ? Plan->CodeVA : 0;
}

uint64_t ELFPatcher::appendExecSegment(std::vector<uint8_t> &Binary,
                                       llvm::ArrayRef<uint8_t> Code,
                                       llvm::StringRef /*SegName*/,
                                       Arch TargetArch) {
  return appendPatchSegments(Binary, Code, TargetArch, std::nullopt);
}

uint64_t ELFPatcher::appendPatchSegments(std::vector<uint8_t> &Binary,
                                         llvm::ArrayRef<uint8_t> Code,
                                         Arch TargetArch,
                                         std::optional<uint64_t> WritableVA,
                                         bool Executable) {
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;
  const uint64_t PageSize = elfPageSize(TargetArch);

  const bool Is64 = Layout.Is64;
  const uint16_t PhdrSize = getELFPhdrSize(Is64);
  auto EHdr = parseELFHeader(Binary.data(), Binary.size());
  uint64_t OldPhOff = EHdr.PhOff;
  uint16_t OldPhNum = EHdr.PhNum;

  const uint64_t TextSize = Code.size();
  const std::optional<ExecSegmentPlan> Plan = planExecSegment(
      Is64, Layout.MaxVA, Binary.size(), OldPhNum, PhdrSize, PageSize, TextSize,
      InputKind != RewriteInputKind::LiftedIR);
  if (!Plan)
    return 0;
  if (WritableVA && (InputKind == RewriteInputKind::LiftedIR ||
                     *WritableVA % PageSize || *WritableVA <= Plan->CodeVA ||
                     *WritableVA - Plan->CodeVA >= Code.size()))
    return 0;

  std::vector<uint8_t> OldPhdrs(static_cast<size_t>(OldPhNum) * PhdrSize);
  // OldPhOff is the untrusted e_phoff: compare against remaining space so a
  // crafted offset cannot wrap the check and read the old headers out of
  // bounds.
  if (OldPhOff > Binary.size() || OldPhdrs.size() > Binary.size() - OldPhOff)
    return 0;
  std::memcpy(OldPhdrs.data(), Binary.data() + OldPhOff, OldPhdrs.size());

  // Code may be a view into Binary.  Preserve only that uncommon case before
  // resize invalidates the caller's view; external code buffers remain
  // zero-copy.
  std::vector<uint8_t> AliasedCode;
  if (rangesOverlap(Code, llvm::ArrayRef<uint8_t>(Binary))) {
    AliasedCode.assign(Code.begin(), Code.end());
    Code = AliasedCode;
  }

  Binary.resize(static_cast<size_t>(Plan->OutputSize), 0);
  if (!Code.empty())
    std::memcpy(Binary.data() + Plan->CodeFileOffset, Code.data(),
                static_cast<size_t>(TextSize));

  uint8_t NewPhdr[sizeof(llvm::object::ELF64LE::Phdr)] = {};
  ELFPhdrFields NewPF;
  NewPF.Type = llvm::ELF::PT_LOAD;
  NewPF.Flags = llvm::ELF::PF_R | (Executable ? llvm::ELF::PF_X : 0);
  NewPF.Offset = Plan->SegmentFileOffset;
  NewPF.VAddr = Plan->SegmentVA;
  NewPF.PAddr = Plan->SegmentVA;
  NewPF.FileSz = WritableVA ? *WritableVA - Plan->SegmentVA : Plan->FileSize;
  // All bytes in this RX segment are file-backed. Advertising page padding
  // as BSS asks the OS loader to zero a non-writable mapping.
  NewPF.MemSz = NewPF.FileSz;
  NewPF.Align = PageSize;
  writeELFPhdr(NewPhdr, Is64, NewPF);

  std::memcpy(Binary.data() + Plan->SegmentFileOffset, OldPhdrs.data(),
              OldPhdrs.size());
  std::memcpy(Binary.data() + Plan->SegmentFileOffset + OldPhdrs.size(),
              NewPhdr, PhdrSize);
  if (WritableVA) {
    ELFPhdrFields Data = NewPF;
    const uint64_t Offset = *WritableVA - Plan->SegmentVA;
    Data.Flags = llvm::ELF::PF_R | llvm::ELF::PF_W;
    Data.Offset = Plan->SegmentFileOffset + Offset;
    Data.VAddr = Data.PAddr = *WritableVA;
    Data.FileSz = Data.MemSz = Plan->FileSize - Offset;
    writeELFPhdr(Binary.data() + Plan->SegmentFileOffset + OldPhdrs.size() +
                     PhdrSize,
                 Is64, Data);
  }

  for (uint16_t I = 0; I < Plan->NewPhNum; ++I) {
    uint8_t *PH = Binary.data() + Plan->SegmentFileOffset + I * PhdrSize;
    auto F = readELFPhdr(PH, Is64);
    if (F.Type == llvm::ELF::PT_PHDR) {
      F.Offset = Plan->SegmentFileOffset;
      F.VAddr = Plan->SegmentVA;
      F.PAddr = Plan->SegmentVA;
      F.FileSz = Plan->PhdrTableSize;
      F.MemSz = Plan->PhdrTableSize;
      writeELFPhdr(PH, Is64, F);
    }
  }

  setELFPhdrTable(Binary.data(), Is64, Plan->SegmentFileOffset, Plan->NewPhNum);
  return Plan->CodeVA;
}

PatchResult ELFPatcher::patch(const std::filesystem::path &InputPath,
                              const std::filesystem::path &OutputPath,
                              llvm::Module &Mod, Arch TargetArch) {
  if (!archELFPatchSupported(TargetArch)) {
    llvm::WithColor::error()
        << "elf_patch: unsupported arch " << getArchName(TargetArch) << "\n";
    return PatchResult{};
  }
  if (!isSingleInstructionMode(CachedMode) &&
      !(CachedMode == InstructionMode::MixedARMThumb &&
        TargetArch == Arch::ARM)) {
    llvm::WithColor::error()
        << "elf_patch: rewriting requires a single instruction mode (got "
        << getInstructionModeName(CachedMode) << ")\n";
    return PatchResult{};
  }

  std::vector<PatchedFunctionEntry> InstalledFunctions;
  PatchResult Patch = readPatchWrite(
      InputPath, OutputPath, /*SetExecPerm=*/true, "elf_patch",
      [&](std::vector<uint8_t> &Binary, PatchResult &Result) -> bool {
        PatchLayout Layout;
        if (!parseLayout(Binary, Layout))
          return false;

        auto CompileMod = llvm::CloneModule(Mod);
        SourceFunctionPreparation SourcePreparation;
        std::string SourceDetail;
        if (!prepareSourceFunctionsForPatch(*CompileMod, CachedImage,
                                            SourcePreparation, SourceDetail)) {
          llvm::WithColor::error()
              << "elf_patch: source identity preparation failed: "
              << SourceDetail << "\n";
          return false;
        }
        if (SourcePreparation.isExactNoOp()) {
          Result.Success = true;
          return true;
        }

        uint64_t CodeStartVA = plannedExecSegmentVA(Binary, TargetArch);
        if (CodeStartVA == 0) {
          llvm::WithColor::error() << "elf_patch: cannot plan exec segment\n";
          return false;
        }

        auto SerializeResolvedCode =
            [&](uint64_t VA, bool IsCode) -> std::optional<uint64_t> {
          if (!IsCode)
            return VA;
          if (CachedImage) {
            const auto Mode = CachedImage->instructionModeAt(VA);
            if (!Mode)
              return std::nullopt;
            return serializeCodePointer(VA, TargetArch, *Mode);
          }
          return serializeCodePointer(VA, TargetArch, CachedMode);
        };
        auto IsExecutable = [&](uint64_t VA) {
          return CachedImage && CachedImage->isCodeAddress(VA);
        };
        std::optional<std::string> SymbolResolutionFailure;
        auto Resolve =
            [&](const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request)
            -> std::optional<uint64_t> {
          std::string Name = Request.Symbol.str();
          if (InputKind == RewriteInputKind::ReplacementIR &&
              TargetArch == Arch::X86 && Name == "_GLOBAL_OFFSET_TABLE_")
            return CodeStartVA;
          const std::string PreservedKey = resolveSourceFunctionAlias(
              Name, SourcePreparation.PreservedOriginalVAs, BinaryFormat::ELF,
              TargetArch);
          if (!PreservedKey.empty())
            return SerializeResolvedCode(
                SourcePreparation.PreservedOriginalVAs.at(PreservedKey), true);
          if (InputKind == RewriteInputKind::ReplacementIR) {
            if (SymbolResolutionFailure || !CachedImage)
              return std::nullopt;
            auto Resolved =
                resolveELFSourceSymbol(*CachedImage, *CompileMod, Request);
            if (!Resolved) {
              SymbolResolutionFailure = llvm::toString(Resolved.takeError());
              return std::nullopt;
            }
            return *Resolved;
          }
          if (CachedExports) {
            for (auto &E : *CachedExports)
              if (E.Name == Name)
                return SerializeResolvedCode(E.Addr, IsExecutable(E.Addr));
          }
          if (CachedImports) {
            for (auto &I : *CachedImports)
              if (I.Name == Name)
                return SerializeResolvedCode(I.IATAddr,
                                             IsExecutable(I.IATAddr));
          }
          if (CachedSymbols) {
            for (const auto &S : *CachedSymbols)
              if (S.IsFunc && S.Name == Name)
                return SerializeResolvedCode(S.Addr, true);
          }
          if (CachedImage && isSynthesizedFuncName(Name)) {
            llvm::StringRef Spelling(Name);
            if (Spelling.consume_front(kAutoFuncPrefix)) {
              va_t Address = 0;
              if (!Spelling.getAsInteger(16, Address) &&
                  CachedImage->hasAuthenticatedFunctionEntryAt(Address))
                return SerializeResolvedCode(Address, true);
            }
          }
          if (auto VA = parseNdDataSymbol(Name)) {
            return CachedImage
                       ? serializeImageDataSymbolAddress(*CachedImage, *VA)
                       : *VA;
          }
          if (auto VA = parseNdCodePtrSymbol(Name))
            return SerializeResolvedCode(*VA, false);
          return std::nullopt;
        };

        // The ELF unwinder reaches a function's FDE only through the sorted
        // table in `.eh_frame_hdr`, so regenerated records are compiled into
        // the file-backed tail of the existing `.eh_frame` -- pinning that
        // section's VA there makes their PC-relative fields resolve against the
        // address they will be registered at -- and their functions are then
        // added to the search table below.
        std::optional<ELFEHFrameRegion> EHRegion = findELFEHFrameRegion(Binary);
        auto FixedSectionVA =
            [&](llvm::StringRef Name) -> std::optional<uint64_t> {
          // A compiler's section alignment need not match the old table's
          // logical end (a four-byte terminator often follows an eight-byte
          // aligned section). Source replacements use the separately placed
          // records and the relocated search-table path below.
          if (EHRegion && Name == section_names::elf::EhFrame &&
              !(InputKind == RewriteInputKind::ReplacementIR &&
                EHRegion->HasHdr))
            return EHRegion->AppendVA;
          return std::nullopt;
        };

        // A 32-bit ARM runtime reaches a frame through `.ARM.exidx` rather
        // than through the DWARF records `.eh_frame_hdr` indexes, so an EHABI
        // image's regenerated unwind information goes into its index instead.
        // The descriptors that index names need no fixed VA of their own: they
        // ride along in the appended segment, and only the eight-byte entries
        // that reach them have to fit in the image's own sorted table.
        std::optional<ELFARMEHABIRegion> EHABIRegion =
            findELFARMEHABIRegion(Binary);

        const auto *MIPSProfile =
            CachedImage && CachedImage->MIPS ? &*CachedImage->MIPS : nullptr;
        CompiledImage Img = compileImageForPatchWithFixedSectionVAs(
            *CompileMod, TargetArch, BinaryFormat::ELF, CodeStartVA, Resolve,
            FixedSectionVA, 0,
            InputKind == RewriteInputKind::ReplacementIR
                ? CompileMod->getTargetTriple().str()
                : std::string(),
            InputKind, MIPSProfile);
        if (SymbolResolutionFailure) {
          llvm::WithColor::error() << *SymbolResolutionFailure << "\n";
          return false;
        }
        if (!Img.Success || Img.Bytes.empty()) {
          llvm::WithColor::error()
              << "elf_patch: compileImageForPatch failed (success="
              << Img.Success << ", code bytes=" << Img.Bytes.size()
              << ", sections=" << Img.Sections.size() << ")\n";
          return false;
        }
        if (CachedImage &&
            !repairMixedARMInterworkingCalls(Img, *CachedImage, SourceDetail)) {
          llvm::WithColor::error()
              << "elf_patch: ARM interworking validation failed: "
              << SourceDetail << "\n";
          return false;
        }

        // The two models are alternatives, and which one applies is settled by
        // what codegen produced: registering the DWARF records of an EHABI
        // image would fail closed over a table its unwinder never reads, and
        // registering an index the compile never emitted would fail closed
        // over records that are perfectly good.
        llvm::Error Err =
            hasGeneratedELFARMEHABI(Img)
                ? installELFARMEHABI(Binary, EHABIRegion, Img, *CompileMod)
                : installELFEHFrame(Binary, EHRegion, Img, *CompileMod);

        // A toolchain may place another section right after `.eh_frame`,
        // leaving no tail for the regenerated records.  Compile them into the
        // patch image instead, and publish a relocated `.eh_frame_hdr` after
        // them whose search table names the original and regenerated records.
        std::optional<uint64_t> RelocatedHdrVA;
        uint64_t RelocatedHdrSize = 0;
        std::optional<uint64_t> RelocatedIndexVA;
        uint64_t RelocatedIndexSize = 0;
        if (Err && !hasGeneratedELFARMEHABI(Img) && EHRegion &&
            EHRegion->HasHdr) {
          const std::string InPlaceFailure = llvm::toString(std::move(Err));
          Img = compileImageForPatchWithFixedSectionVAs(
              *CompileMod, TargetArch, BinaryFormat::ELF, CodeStartVA, Resolve,
              [](llvm::StringRef) -> std::optional<uint64_t> {
                return std::nullopt;
              },
              0,
              InputKind == RewriteInputKind::ReplacementIR
                  ? CompileMod->getTargetTriple().str()
                  : std::string(),
              InputKind, MIPSProfile);
          if (!Img.Success || Img.Bytes.empty()) {
            llvm::WithColor::error()
                << "elf_patch: compileImageForPatch failed after "
                << InPlaceFailure << "\n";
            return false;
          }
          if (CachedImage && !repairMixedARMInterworkingCalls(Img, *CachedImage,
                                                              SourceDetail)) {
            llvm::WithColor::error()
                << "elf_patch: ARM interworking validation failed: "
                << SourceDetail << "\n";
            return false;
          }
          auto Storage =
              InputKind == RewriteInputKind::ReplacementIR
                  ? validatePatchStorageLayout(Img, elfPageSize(TargetArch))
                  : llvm::Expected<PatchStorageLayout>(PatchStorageLayout{});
          if (!Storage) {
            llvm::WithColor::error()
                << llvm::toString(Storage.takeError()) << "\n";
            return false;
          }
          const uint64_t HdrOffset =
              llvm::alignTo(Storage->WritableOffset ? Storage->ReadOnlySize
                                                    : Img.Bytes.size(),
                            dweh::kEhFrameHdrAlignment);
          auto Hdr = buildRelocatedELFEHFrameHdr(
              Binary, *EHRegion, Img, *CompileMod, Img.BaseVA + HdrOffset);
          if (!Hdr) {
            llvm::WithColor::error() << InPlaceFailure << "; "
                                     << llvm::toString(Hdr.takeError()) << "\n";
            return false;
          }
          if (Storage->WritableOffset) {
            if (!rangeInBounds(HdrOffset, Hdr->size(),
                               Storage->WritableOffset)) {
              llvm::WithColor::error() << "elf_patch: unwind search table "
                                          "exceeds reserved RX storage\n";
              return false;
            }
            std::copy(Hdr->begin(), Hdr->end(), Img.Bytes.begin() + HdrOffset);
          } else {
            Img.Bytes.resize(static_cast<size_t>(HdrOffset), 0);
            Img.Bytes.insert(Img.Bytes.end(), Hdr->begin(), Hdr->end());
          }
          RelocatedHdrVA = Img.BaseVA + HdrOffset;
          RelocatedHdrSize = Hdr->size();
        } else if (Err && hasGeneratedELFARMEHABI(Img) && EHABIRegion &&
                   InputKind == RewriteInputKind::ReplacementIR) {
          const std::string InPlaceFailure = llvm::toString(std::move(Err));
          auto Storage =
              validatePatchStorageLayout(Img, elfPageSize(TargetArch));
          if (!Storage) {
            llvm::WithColor::error()
                << llvm::toString(Storage.takeError()) << "\n";
            return false;
          }
          const uint64_t Offset =
              llvm::alignTo(Storage->WritableOffset ? Storage->ReadOnlySize
                                                    : Img.Bytes.size(),
                            4);
          auto Index = buildRelocatedELFARMEHABIIndex(Binary, *EHABIRegion, Img,
                                                      Img.BaseVA + Offset);
          if (!Index) {
            llvm::WithColor::error()
                << InPlaceFailure << "; " << llvm::toString(Index.takeError())
                << "\n";
            return false;
          }
          if (Storage->WritableOffset) {
            if (!rangeInBounds(Offset, Index->size(),
                               Storage->WritableOffset)) {
              llvm::WithColor::error() << "elf_patch: EHABI index exceeds "
                                          "reserved read-only storage\n";
              return false;
            }
            std::copy(Index->begin(), Index->end(), Img.Bytes.begin() + Offset);
          } else {
            Img.Bytes.resize(Offset, 0);
            Img.Bytes.insert(Img.Bytes.end(), Index->begin(), Index->end());
          }
          RelocatedIndexVA = Img.BaseVA + Offset;
          RelocatedIndexSize = Index->size();
        } else if (Err) {
          llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
          return false;
        }

        uint64_t TextSize = Img.Bytes.size();
        std::optional<uint64_t> WritableVA;
        ELFSourceRelocationTable Relocations;
        if (InputKind == RewriteInputKind::ReplacementIR) {
          auto Storage =
              validatePatchStorageLayout(Img, elfPageSize(TargetArch));
          if (!Storage) {
            llvm::WithColor::error()
                << llvm::toString(Storage.takeError()) << "\n";
            return false;
          }
          if (Storage->WritableOffset)
            WritableVA = Img.BaseVA + Storage->WritableOffset;
          uint64_t MetadataOffset = Storage->ReadOnlySize;
          if (RelocatedHdrVA)
            MetadataOffset =
                std::max(MetadataOffset,
                         *RelocatedHdrVA - Img.BaseVA + RelocatedHdrSize);
          if (RelocatedIndexVA)
            MetadataOffset =
                std::max(MetadataOffset,
                         *RelocatedIndexVA - Img.BaseVA + RelocatedIndexSize);
          auto Table = prepareELFSourceRelocations(*CachedImage, *CompileMod,
                                                   Img, MetadataOffset);
          if (!Table) {
            llvm::WithColor::error()
                << llvm::toString(Table.takeError()) << "\n";
            return false;
          }
          Relocations = *Table;
        }
        uint64_t Placed =
            appendPatchSegments(Binary, Img.Bytes, TargetArch, WritableVA);
        if (Placed == 0) {
          llvm::WithColor::error() << "elf_patch: appendExecSegment failed\n";
          return false;
        }
        if (RelocatedHdrVA) {
          llvm::Error Retarget =
              Placed == Img.BaseVA
                  ? retargetELFEHFrameHdr(Binary, *RelocatedHdrVA,
                                          RelocatedHdrSize)
                  : llvm::createStringError(
                        llvm::errc::invalid_argument,
                        "elf_patch: patch image moved from its compiled VA");
          if (Retarget) {
            llvm::WithColor::error()
                << llvm::toString(std::move(Retarget)) << "\n";
            return false;
          }
        }
        if (RelocatedIndexVA) {
          if (Placed != Img.BaseVA) {
            llvm::WithColor::error() << "elf_patch: EHABI image moved\n";
            return false;
          }
          if (auto Error = retargetELFARMEHABIIndex(Binary, *RelocatedIndexVA,
                                                    RelocatedIndexSize, &Img)) {
            llvm::WithColor::error()
                << llvm::toString(std::move(Error)) << "\n";
            return false;
          }
        }

        if (InputKind == RewriteInputKind::ReplacementIR) {
          if (auto Error = installELFSourceRelocations(Binary, Relocations)) {
            llvm::WithColor::error()
                << llvm::toString(std::move(Error)) << "\n";
            return false;
          }
          if (auto Error = registerELFPatchSections(Binary, Img.Sections)) {
            llvm::WithColor::error()
                << llvm::toString(std::move(Error)) << "\n";
            return false;
          }
        }

        if (!Img.Unresolved.empty()) {
          llvm::WithColor::warning() << "elf_patch: " << Img.Unresolved.size()
                                     << " unresolved symbols\n";
        }

        const std::vector<Export> AuthenticatedExports =
            authenticatedFunctionExports(CachedImage);
        const std::vector<Export> *TrampolineExports =
            CachedImage ? &AuthenticatedExports : CachedExports;
        const bool HasExactSourcePlan = SourcePreparation.HasExactSources;
        if (HasExactSourcePlan &&
            !validateSourceFunctionPatchPlan(Img, CachedImage, SourceDetail)) {
          llvm::WithColor::error()
              << "elf_patch: source identity validation failed: "
              << SourceDetail << "\n";
          return false;
        }
        const SourceTrampolinePlan TrampolinePlan =
            HasExactSourcePlan ? makeSourceTrampolinePlan(Img, CachedImage)
                               : SourceTrampolinePlan{};
        if (HasExactSourcePlan && TrampolinePlan.PreservedCount != 0) {
          llvm::WithColor::error()
              << "elf_patch: an unsafe source definition escaped the "
                 "preservation prepass\n";
          return false;
        }

        size_t TrampCount = 0;
        if (HasExactSourcePlan) {
          auto Regions = collectSourceTrampolineRegions(
              Binary, *CachedImage, TrampolinePlan.OriginalVAs);
          if (!Regions) {
            llvm::WithColor::error()
                << llvm::toString(Regions.takeError()) << '\n';
            return false;
          }
          for (const auto &Region : *Regions)
            TrampCount += installTrampolines(
                Binary, Img.SymbolAddrs, Region.SectionVA, Region.SectionSize,
                Region.SectionFileoff, /*ImageBase=*/0, TargetArch, CachedMode,
                CachedSymbols, CachedCodeRanges, TrampolineExports,
                /*PatchedOriginalEntries=*/nullptr,
                /*PatchedEntryMappings=*/nullptr, &InstalledFunctions,
                TrampolinePlan.Owners, TrampolinePlan.OriginalVAs, CachedImage,
                InputKind == RewriteInputKind::ReplacementIR
                    ? &Img.ReplacementFunctionModes
                    : nullptr);
          if (!validatePatchedSourceTrampolineClosure(
                  TrampolinePlan, InstalledFunctions, TrampCount,
                  SourceDetail)) {
            llvm::WithColor::error()
                << "elf_patch: source trampoline closure failed: "
                << SourceDetail << "\n";
            return false;
          }
        } else {
          TrampCount = installTrampolines(
              Binary, Img.SymbolAddrs, Layout.TextVA, Layout.TextSize,
              Layout.TextFileOff, /*ImageBase=*/0, TargetArch, CachedMode,
              CachedSymbols, CachedCodeRanges, TrampolineExports,
              /*PatchedOriginalEntries=*/nullptr,
              /*PatchedEntryMappings=*/nullptr, &InstalledFunctions,
              CachedImage);
        }

        Result.Success = true;
        Result.CodeSize = TextSize;
        Result.TrampolineCount = TrampCount;
        return true;
      });
  patch_receipt_detail::publishCommitted(
      Patch, InstalledFunctions, [](const PatchedFunctionEntry &Installed) {
        return Installed.OriginalVA;
      });
  if (Patch.Success)
    Patch.PatchedFunctions = std::move(InstalledFunctions);
  return Patch;
}

} // namespace neverd
