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

#include <cstring>
#include <optional>

#define DEBUG_TYPE "neverd-elf-patch"

namespace neverd {

bool ELFPatcher::parseLayout(const std::vector<uint8_t> &Data,
                             PatchLayout &Layout) {
  auto Hdr = parseELFHeader(Data.data(), Data.size());
  if (Hdr.PhNum == 0) {
    llvm::WithColor::error() << "elf_patch: not a valid ELF file\n";
    return false;
  }

  Layout.Is64 = Hdr.Is64;
  Layout.EntryPoint = Hdr.EntryPoint;
  Layout.PhNum = Hdr.PhNum;
  Layout.PhOff = Hdr.PhOff;
  Layout.MaxVA = 0;
  Layout.MaxFileOff = 0;

  forEachELFPhdr(Data.data(), Data.size(),
                 [&](const ELFPhdrFields &F, const uint8_t *, bool) {
                   if (F.Type != llvm::ELF::PT_LOAD)
                     return;

                   uint64_t EndVA = F.VAddr + F.MemSz;
                   uint64_t EndFile = F.Offset + F.FileSz;
                   if (EndVA > Layout.MaxVA)
                     Layout.MaxVA = EndVA;
                   if (EndFile > Layout.MaxFileOff)
                     Layout.MaxFileOff = EndFile;

                   if (F.Flags & llvm::ELF::PF_X) {
                     Layout.TextVA = F.VAddr;
                     Layout.TextSize = F.MemSz;
                     Layout.TextFileOff = F.Offset;
                     Layout.TextFileSize = F.FileSz;
                   }
                 });

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
  uint64_t NewSegVA = alignUp(Layout.MaxVA, PageSize);
  uint64_t PhdrTableSz =
      static_cast<uint64_t>(EHdr.PhNum + 1) * getELFPhdrSize(Layout.Is64);
  return NewSegVA + PhdrTableSz;
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

  uint64_t NewSegFileOff = alignUp(Binary.size(), PageSize);
  uint64_t NewSegVA = alignUp(Layout.MaxVA, PageSize);

  // e_phnum is a uint16 field; if it is already at the maximum, adding our new
  // PT_LOAD would wrap NewPhNum to 0, collapse PhdrTableSz, and later memcpy
  // the old program-header table (sized from OldPhNum) past the resized buffer.
  if (OldPhNum >= 0xFFFF)
    return 0;
  uint16_t NewPhNum = OldPhNum + 1;
  uint64_t PhdrTableSz = static_cast<uint64_t>(NewPhNum) * PhdrSize;
  uint64_t CodeStartVA = NewSegVA + PhdrTableSz;

  uint64_t TextSize = Code.size();
  uint64_t CodeStartOff = NewSegFileOff + PhdrTableSz;
  uint64_t TotalFileSz = PhdrTableSz + TextSize;
  uint64_t TotalMemSz = alignUp(TotalFileSz, PageSize);

  std::vector<uint8_t> OldPhdrs(static_cast<size_t>(OldPhNum) * PhdrSize);
  // OldPhOff is the untrusted e_phoff: compare against remaining space so a
  // crafted offset cannot wrap the check and read the old headers out of
  // bounds.
  if (OldPhOff <= Binary.size() && OldPhdrs.size() <= Binary.size() - OldPhOff)
    std::memcpy(OldPhdrs.data(), Binary.data() + OldPhOff, OldPhdrs.size());

  Binary.resize(static_cast<size_t>(NewSegFileOff + TotalMemSz), 0);
  std::memcpy(Binary.data() + CodeStartOff, Code.data(),
              static_cast<size_t>(TextSize));

  uint8_t NewPhdr[sizeof(llvm::object::ELF64LE::Phdr)] = {};
  ELFPhdrFields NewPF;
  NewPF.Type = llvm::ELF::PT_LOAD;
  NewPF.Flags = llvm::ELF::PF_R | llvm::ELF::PF_X;
  NewPF.Offset = NewSegFileOff;
  NewPF.VAddr = NewSegVA;
  NewPF.PAddr = NewSegVA;
  NewPF.FileSz = TotalFileSz;
  NewPF.MemSz = TotalMemSz;
  NewPF.Align = PageSize;
  writeELFPhdr(NewPhdr, Is64, NewPF);

  std::memcpy(Binary.data() + NewSegFileOff, OldPhdrs.data(), OldPhdrs.size());
  std::memcpy(Binary.data() + NewSegFileOff + OldPhdrs.size(), NewPhdr,
              PhdrSize);

  for (uint16_t I = 0; I < NewPhNum; ++I) {
    uint8_t *PH = Binary.data() + NewSegFileOff + I * PhdrSize;
    auto F = readELFPhdr(PH, Is64);
    if (F.Type == llvm::ELF::PT_PHDR) {
      F.Offset = NewSegFileOff;
      F.VAddr = NewSegVA;
      F.PAddr = NewSegVA;
      F.FileSz = PhdrTableSz;
      F.MemSz = PhdrTableSz;
      writeELFPhdr(PH, Is64, F);
    }
  }

  setELFPhdrTable(Binary.data(), Is64, NewSegFileOff, NewPhNum);
  return CodeStartVA;
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
