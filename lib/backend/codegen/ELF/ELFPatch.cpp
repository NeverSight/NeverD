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
#include "neverd/backend/codegen/ELF/ELFARMEHABIPatch.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

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
                uint64_t CodeSize) {
  if (OldPhNum >= std::numeric_limits<uint16_t>::max())
    return std::nullopt;

  ExecSegmentPlan Plan;
  Plan.NewPhNum = OldPhNum + 1;
  Plan.PhdrTableSize = static_cast<uint64_t>(Plan.NewPhNum) * PhdrSize;

  const std::optional<uint64_t> SegmentFileOffset =
      checkedAlignUp(BinarySize, PageSize);
  const std::optional<uint64_t> SegmentVA = checkedAlignUp(MaxVA, PageSize);
  if (!SegmentFileOffset || !SegmentVA)
    return std::nullopt;
  Plan.SegmentFileOffset = *SegmentFileOffset;
  Plan.SegmentVA = *SegmentVA;

  const std::optional<uint64_t> CodeFileOffset =
      checkedAdd(Plan.SegmentFileOffset, Plan.PhdrTableSize);
  const std::optional<uint64_t> CodeVA =
      checkedAdd(Plan.SegmentVA, Plan.PhdrTableSize);
  const std::optional<uint64_t> FileSize =
      checkedAdd(Plan.PhdrTableSize, CodeSize);
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
                      getELFPhdrSize(Layout.Is64), PageSize, /*CodeSize=*/0);
  return Plan ? Plan->CodeVA : 0;
}

uint64_t ELFPatcher::appendExecSegment(std::vector<uint8_t> &Binary,
                                       llvm::ArrayRef<uint8_t> Code,
                                       llvm::StringRef /*SegName*/,
                                       Arch TargetArch) {
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
  const std::optional<ExecSegmentPlan> Plan =
      planExecSegment(Is64, Layout.MaxVA, Binary.size(), OldPhNum, PhdrSize,
                      PageSize, TextSize);
  if (!Plan)
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
  NewPF.Flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
  NewPF.Offset = Plan->SegmentFileOffset;
  NewPF.VAddr = Plan->SegmentVA;
  NewPF.PAddr = Plan->SegmentVA;
  NewPF.FileSz = Plan->FileSize;
  NewPF.MemSz = Plan->MemorySize;
  NewPF.Align = PageSize;
  writeELFPhdr(NewPhdr, Is64, NewPF);

  std::memcpy(Binary.data() + Plan->SegmentFileOffset, OldPhdrs.data(),
              OldPhdrs.size());
  std::memcpy(Binary.data() + Plan->SegmentFileOffset + OldPhdrs.size(),
              NewPhdr, PhdrSize);

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

  return readPatchWrite(
      InputPath, OutputPath, /*SetExecPerm=*/true, "elf_patch",
      [&](std::vector<uint8_t> &Binary, PatchResult &Result) -> bool {
        PatchLayout Layout;
        if (!parseLayout(Binary, Layout))
          return false;

        uint64_t CodeStartVA = plannedExecSegmentVA(Binary, TargetArch);
        if (CodeStartVA == 0) {
          llvm::WithColor::error() << "elf_patch: cannot plan exec segment\n";
          return false;
        }

        InstructionMode ResolveMode = CachedMode;
        auto SerializeResolvedCode = [&](uint64_t VA, bool IsCode) {
          return IsCode ? serializeCodePointer(VA, TargetArch, ResolveMode)
                        : VA;
        };
        auto IsExecutable = [&](uint64_t VA) {
          const Segment *Seg =
              CachedImage ? CachedImage->getSegmentFor(VA) : nullptr;
          return Seg && Seg->isExecutable();
        };
        auto Resolve = [&](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
          std::string Name = Sym.str();
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
          if (auto VA = parseNdDataSymbol(Name)) {
            bool IsCode = IsExecutable(*VA) && CachedImage &&
                          CachedImage->CodeRefTargets.count(*VA) != 0;
            return SerializeResolvedCode(*VA, IsCode);
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
          if (EHRegion && Name == section_names::elf::EhFrame)
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

        CompiledImage Img = compileImageForPatchWithFixedSectionVAs(
            Mod, TargetArch, BinaryFormat::ELF, CodeStartVA, Resolve,
            FixedSectionVA);
        if (!Img.Success || Img.Bytes.empty()) {
          llvm::WithColor::error()
              << "elf_patch: compileImageForPatch failed\n";
          return false;
        }

        // The two models are alternatives, and which one applies is settled by
        // what codegen produced: registering the DWARF records of an EHABI
        // image would fail closed over a table its unwinder never reads, and
        // registering an index the compile never emitted would fail closed
        // over records that are perfectly good.
        llvm::Error Err =
            hasGeneratedELFARMEHABI(Img)
                ? installELFARMEHABI(Binary, EHABIRegion, Img, Mod)
                : installELFEHFrame(Binary, EHRegion, Img, Mod);
        if (Err) {
          llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
          return false;
        }

        uint64_t TextSize = Img.Bytes.size();
        uint64_t Placed =
            appendExecSegment(Binary, Img.Bytes, kNdTextSection, TargetArch);
        if (Placed == 0) {
          llvm::WithColor::error() << "elf_patch: appendExecSegment failed\n";
          return false;
        }

        if (!Img.Unresolved.empty()) {
          llvm::WithColor::warning() << "elf_patch: " << Img.Unresolved.size()
                                     << " unresolved symbols\n";
        }

        size_t TrampCount =
            installTrampolines(Binary, Img.SymbolAddrs, Layout.TextVA,
                               Layout.TextSize, Layout.TextFileOff,
                               /*ImageBase=*/0, TargetArch, CachedMode,
                               CachedSymbols, CachedCodeRanges, CachedExports);

        Result.Success = true;
        Result.CodeSize = TextSize;
        Result.TrampolineCount = TrampCount;
        return true;
      });
}

} // namespace neverd
