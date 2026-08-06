//===- COFFPatch.cpp - COFF/PE binary patching -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE binary patching implementation.  Handles both PE32 and PE32+
/// in a single unified code path — the Is64 flag in the parsed layout
/// drives pointer-size and optional-header differences.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFPatch.h"

#include "neverd/ArchSupport.h"
#include "neverd/Object/PELayout.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/COFF/COFFReloc.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>

#define DEBUG_TYPE "neverd-coff-patch"

namespace neverd {

bool COFFPatcher::parseLayout(const std::vector<uint8_t> &Data,
                              PatchLayout &Layout) {
  auto PE = locatePEHeaders(const_cast<uint8_t *>(Data.data()), Data.size());
  if (!PE.valid()) {
    llvm::WithColor::error() << "coff_patch: not a valid PE file\n";
    return false;
  }

  Layout.PeOffset = PE.PeOffset;
  Layout.Is64 = PE.Is64;
  Layout.NumSections = PE.NumSections;
  Layout.OptionalHdrSize = PE.FileHeader->SizeOfOptionalHeader;
  Layout.SectionTableOff = static_cast<uint32_t>(PE.SectionTable - Data.data());

  Layout.ImageBase = getPEImageBase(PE);
  Layout.SectionAlignment = getPESectionAlignment(PE);
  Layout.FileAlignment = getPEFileAlignment(PE);
  Layout.SizeOfImage = getPESizeOfImage(PE);
  Layout.SizeOfHeaders = getPESizeOfHeaders(PE);
  Layout.EntryPointRva = getPEAddressOfEntryPoint(PE);

  // Locate the original code section so trampolines can be written over the
  // functions being replaced.  Resolution order:
  //   1. user-forced name ("--text-section .vmp0" for a packed/renamed PE),
  //   2. the canonical ".text",
  //   3. flag-based fallback: the executable section containing the entry
  //      point, else the largest executable section.
  // Step 3 mirrors BinaryImage::getTextSection() so section-mode patching keeps
  // working on a binary whose code section was renamed by a packer/protector
  // (VMProtect ".vmp0", UPX "UPX1", Themida, randomised names) even without an
  // explicit --text-section.  Without any match Layout.Text* stay 0 and
  // installTrampolines() would skip every function, silently producing a binary
  // with no redirection.
  PESectionFields TextSec;
  bool FoundText = false;
  if (!TextSectionOverride.empty())
    FoundText = findPESection(PE, TextSectionOverride, TextSec);
  if (!FoundText)
    FoundText = findPESection(PE, section_names::coff::Text, TextSec);
  if (!FoundText) {
    // A packer can zero a section's VirtualSize; the Windows loader then treats
    // the virtual extent as SizeOfRawData.  Use that effective size for both
    // the entry-containment test and the "largest" comparison, otherwise a
    // zeroed VirtualSize yields TextSize=0 and installTrampolines() silently
    // skips every function — the very failure this fallback exists to prevent.
    auto effSize = [](const PESectionFields &F) -> uint32_t {
      return F.VirtualSize ? F.VirtualSize : F.SizeOfRawData;
    };
    uint32_t EntryRVA = Layout.EntryPointRva;
    PESectionFields EntrySec, BiggestSec;
    bool HasEntry = false, HasBiggest = false;
    forEachPESection(PE, [&](const PESectionFields &F, uint16_t) {
      if (!(F.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
        return;
      if (EntryRVA != 0 && EntryRVA >= F.VirtualAddress &&
          EntryRVA < F.VirtualAddress + effSize(F)) {
        EntrySec = F;
        HasEntry = true;
      }
      if (!HasBiggest || effSize(F) > effSize(BiggestSec)) {
        BiggestSec = F;
        HasBiggest = true;
      }
    });
    if (HasEntry) {
      TextSec = EntrySec;
      FoundText = true;
    } else if (HasBiggest) {
      TextSec = BiggestSec;
      FoundText = true;
    }
  }
  if (FoundText) {
    Layout.TextVA = TextSec.VirtualAddress;
    Layout.TextSize =
        TextSec.VirtualSize ? TextSec.VirtualSize : TextSec.SizeOfRawData;
    Layout.TextFileOff = TextSec.PointerToRawData;
    Layout.TextFileSize = TextSec.SizeOfRawData;
  }

  COFFRelocResolver Resolver;
  if (Resolver.parse(Data, Arch::Unknown)) {
    for (const auto &E : Resolver.entries()) {
      uint64_t RVA = E.Addr - Layout.ImageBase;
      Layout.Imports.push_back({E.Name, RVA});
      Layout.IATMap[E.Name] = RVA;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "coff_patch: parsed " << Layout.NumSections
                          << " sections, " << Layout.Imports.size()
                          << " imports, .text RVA=0x"
                          << llvm::utohexstr(Layout.TextVA) << " size=0x"
                          << llvm::utohexstr(Layout.TextSize) << "\n");
  return true;
}

uint64_t COFFPatcher::plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                           Arch /*TargetArch*/) {
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;
  uint32_t NewSecRva = alignUp(Layout.SizeOfImage, Layout.SectionAlignment);
  return Layout.ImageBase + NewSecRva;
}

uint64_t COFFPatcher::appendExecSegment(std::vector<uint8_t> &Binary,
                                        llvm::ArrayRef<uint8_t> Code,
                                        llvm::StringRef SegName,
                                        Arch /*TargetArch*/) {
  using namespace llvm::object;
  PatchLayout Layout;
  if (!parseLayout(Binary, Layout))
    return 0;

  uint32_t NewSecRva = alignUp(Layout.SizeOfImage, Layout.SectionAlignment);
  uint64_t CodeVA = Layout.ImageBase + NewSecRva;

  uint64_t TextSize = Code.size();
  // FileAlignment/SectionAlignment are untrusted PE fields used here as alignUp
  // divisors: a zero value makes alignUp collapse to 0 (the resize(0) + memcpy
  // below would then write out of bounds), and a huge value can push the 32-bit
  // file offsets past 4 GiB and wrap the resize length.  Validate before use.
  if (Layout.FileAlignment == 0 || Layout.SectionAlignment == 0) {
    llvm::WithColor::error() << "coff_patch: invalid PE alignment\n";
    return 0;
  }
  uint64_t NewSecRawOff64 = alignUp(Binary.size(), Layout.FileAlignment);
  uint64_t NewSecRawSize64 = alignUp(TextSize, Layout.FileAlignment);
  if (NewSecRawOff64 + NewSecRawSize64 > 0xFFFFFFFFULL) {
    llvm::WithColor::error() << "coff_patch: section layout exceeds PE limits\n";
    return 0;
  }
  uint32_t NewSecRawOff = static_cast<uint32_t>(NewSecRawOff64);
  uint32_t NewSecRawSize = static_cast<uint32_t>(NewSecRawSize64);
  uint32_t NewSecVsize =
      alignUp(static_cast<uint32_t>(TextSize), Layout.SectionAlignment);

  Binary.resize(NewSecRawOff + NewSecRawSize, 0);
  std::memcpy(Binary.data() + NewSecRawOff, Code.data(), TextSize);

  uint32_t SecTableOff = Layout.SectionTableOff;
  uint32_t NewSecHdrOff =
      SecTableOff + Layout.NumSections * sizeof(coff_section);
  if (NewSecHdrOff + sizeof(coff_section) > NewSecRawOff) {
    llvm::WithColor::error() << "coff_patch: no room for new section header\n";
    return 0;
  }

  coff_section NewSec = {};
  // COFF short section names are at most 8 bytes.
  std::string Name = SegName.empty() ? kNdTextSection.str() : SegName.str();
  std::memcpy(NewSec.Name, Name.data(), std::min<size_t>(Name.size(), 8));
  NewSec.VirtualSize = static_cast<uint32_t>(TextSize);
  NewSec.VirtualAddress = NewSecRva;
  NewSec.SizeOfRawData = NewSecRawSize;
  NewSec.PointerToRawData = NewSecRawOff;
  NewSec.Characteristics = llvm::COFF::IMAGE_SCN_CNT_CODE |
                           llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                           llvm::COFF::IMAGE_SCN_MEM_READ;
  std::memcpy(Binary.data() + NewSecHdrOff, &NewSec, sizeof(coff_section));

  auto PE2 = locatePEHeaders(Binary.data(), Binary.size());
  if (PE2.valid()) {
    PE2.FileHeader->NumberOfSections =
        static_cast<uint16_t>(Layout.NumSections + 1);
    setPESizeOfImage(PE2, NewSecRva + NewSecVsize);
    clearPEChecksum(PE2);
    clearPEDataDirectory(PE2, llvm::COFF::CERTIFICATE_TABLE);
  }

  return CodeVA;
}

PatchResult COFFPatcher::patch(const std::filesystem::path &InputPath,
                               const std::filesystem::path &OutputPath,
                               llvm::Module &Mod, Arch TargetArch) {
  if (!archCOFFPatchSupported(TargetArch)) {
    llvm::WithColor::error()
        << "coff_patch: unsupported arch " << getArchName(TargetArch) << "\n";
    return PatchResult{};
  }

  return readPatchWrite(
      InputPath, OutputPath, /*SetExecPerm=*/false, "coff_patch",
      [&](std::vector<uint8_t> &Binary, PatchResult &Result) -> bool {
        PatchLayout Layout;
        if (!parseLayout(Binary, Layout))
          return false;
        if (TargetArch == Arch::ARM &&
            CachedMode != InstructionMode::Thumb) {
          llvm::WithColor::error()
              << "coff_patch: Windows ARM patching requires Thumb image "
                 "context\n";
          return false;
        }

        uint64_t CodeVA = plannedExecSegmentVA(Binary, TargetArch);
        if (CodeVA == 0) {
          llvm::WithColor::error() << "coff_patch: cannot plan exec segment\n";
          return false;
        }

        InstructionMode ResolveMode = CachedMode;
        auto SerializeResolvedCode = [&](uint64_t VA, bool IsCode) {
          return IsCode ? serializeCodePointer(VA, TargetArch, ResolveMode)
                        : VA;
        };
        auto IsExecutable = [&](uint64_t VA) {
          const Segment *Seg = CachedImage ? CachedImage->getSegmentFor(VA)
                                           : nullptr;
          return Seg && Seg->isExecutable();
        };
        auto Resolve = [&](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
          std::string Name = Sym.str();
          std::string Key = resolveSymbolAlias(Name, Layout.IATMap);
          auto It = Layout.IATMap.find(Key);
          if (It != Layout.IATMap.end())
            return SerializeResolvedCode(Layout.ImageBase + It->second,
                                         false);
          if (CachedExports) {
            for (auto &E : *CachedExports)
              if (E.Name == Name)
                return CachedImage
                           ? serializeExportAddress(*CachedImage, E.Addr)
                           : E.Addr;
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

        CompiledImage Img = compileImageForPatch(
            Mod, TargetArch, BinaryFormat::COFF, CodeVA, Resolve);
        if (!Img.Success || Img.Bytes.empty()) {
          llvm::WithColor::error()
              << "coff_patch: compileImageForPatch failed\n";
          return false;
        }

        uint64_t TextSize = Img.Bytes.size();
        uint64_t Placed =
            appendExecSegment(Binary, Img.Bytes, kNdTextSection, TargetArch);
        if (Placed == 0) {
          llvm::WithColor::error() << "coff_patch: appendExecSegment failed\n";
          return false;
        }

        if (!Img.Unresolved.empty()) {
          llvm::WithColor::warning() << "coff_patch: " << Img.Unresolved.size()
                                     << " unresolved symbols\n";
        }

        size_t TrampCount = installTrampolines(
            Binary, Img.SymbolAddrs, Layout.TextVA, Layout.TextSize,
            Layout.TextFileOff, Layout.ImageBase, TargetArch, CachedMode,
            CachedSymbols, CachedCodeRanges, CachedExports);

        Result.Success = true;
        Result.CodeSize = TextSize;
        Result.TrampolineCount = TrampCount;
        return true;
      });
}

} // namespace neverd
