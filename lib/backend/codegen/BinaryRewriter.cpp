//===- BinaryRewriter.cpp - Common binary patching logic -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements format-agnostic binary patching operations: text section
/// discovery, trampoline installation, and the read-patch-write skeleton
/// shared by the COFF, ELF, and Mach-O patching pipelines.
///
/// In-place rewriting logic lives in InplaceRewriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/BinaryRewriter.h"

#include "neverd/Object/ELFLayout.h"
#include "neverd/Object/MachOLayout.h"
#include "neverd/Object/PELayout.h"
#include "neverd/Object/SectionNames.h"
#include "neverd/Support/TargetCodegenInfo.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/COFF/COFFInplace.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/ELF/ELFInplace.h"
#include "neverd/backend/codegen/ELF/ELFPatch.h"
#include "neverd/backend/codegen/MachO/MachOInplace.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <filesystem>

#define DEBUG_TYPE "neverd-rewriter"

namespace neverd {

//===----------------------------------------------------------------------===//
// Factory methods (cf. llvm::object::ObjectFile::createObjectFile)
//===----------------------------------------------------------------------===//

std::unique_ptr<BinaryPatcher> BinaryPatcher::create(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::COFF:
    return std::make_unique<COFFPatcher>();
  case BinaryFormat::ELF:
    return std::make_unique<ELFPatcher>();
  case BinaryFormat::MachO:
    return std::make_unique<MachOPatcher>();
  default:
    return nullptr;
  }
}

std::unique_ptr<InplaceRewriter> InplaceRewriter::create(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::COFF:
    return std::make_unique<COFFInplaceRewriter>();
  case BinaryFormat::ELF:
    return std::make_unique<ELFInplaceRewriter>();
  case BinaryFormat::MachO:
    return std::make_unique<MachOInplaceRewriter>();
  default:
    return nullptr;
  }
}

//===----------------------------------------------------------------------===//
// compileImageForPatch — two-pass multi-section image compile
//===----------------------------------------------------------------------===//

CompiledImage compileImageForPatch(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef, uint32_t)>
        ResolveFn) {
  CompiledImage Out;
  Out.BaseVA = BaseVA;

  auto IsText = [](llvm::StringRef N) {
    return section_names::isTextSectionName(N);
  };

  // Pass 1: compile with every section anchored at BaseVA. For the common
  // single-section (text-only) case this is already final — text sits at
  // BaseVA and there are no cross-section fixups, so every fixup value is
  // correct and we return after one compile with no module clone (matching the
  // pre-multi-section fast path). When more than one section is emitted the
  // fixup *values* for cross-section references are stale, but section *sizes*
  // (fixups are fixed-width, applied in place) are exact and drive the layout.
  llvm::mc_rewrite::RewriteOptions Pass1;
  Pass1.Model.TextVA = BaseVA;
  Pass1.Model.getSectionVA = [&](llvm::StringRef) { return BaseVA; };
  Pass1.Model.resolve = [&](llvm::StringRef S, uint32_t Sp) {
    return ResolveFn(S, Sp);
  };
  Codegen CG1;
  auto Res1 = CG1.compileForRewrite(Mod, TargetArch, Pass1, Fmt);
  if (Res1.Sections.empty())
    return Out;

  if (Res1.Sections.size() == 1) {
    Out.Bytes = std::move(Res1.Sections.front().Bytes);
    Out.SymbolAddrs = std::move(Res1.SymbolAddrs);
    Out.Unresolved = std::move(Res1.Unresolved);
    Out.Success = true;
    LLVM_DEBUG(llvm::dbgs()
               << "compileImageForPatch: 1 section, " << Out.Bytes.size()
               << " bytes from VA 0x" << llvm::utohexstr(BaseVA) << "\n");
    return Out;
  }

  // Plan a contiguous layout: text first at BaseVA, then the remaining sections
  // in emission order, each 16-byte aligned (covers 8-byte pointer/blockaddress
  // tables — whose loads use scale8 fixups — and vector constants).  Section
  // spacing uses a per-section MONOTONIC max size (MaxSize, grown each round)
  // rather than this compile's size, so a section never moves backward and the
  // layout cannot oscillate (see the iteration note below).
  const uint64_t Align = 16;
  std::map<std::string, uint64_t> MaxSize;
  auto planLayout =
      [&](const std::vector<llvm::mc_rewrite::RewriteSection> &Secs,
          std::map<std::string, uint64_t> &VA, uint64_t &Total) {
        VA.clear();
        uint64_t Cur = BaseVA;
        auto add = [&](const llvm::mc_rewrite::RewriteSection &S) {
          uint64_t V = alignUp(Cur, Align);
          VA[S.Name] = V;
          Cur = V + MaxSize[S.Name];
        };
        for (auto &S : Secs)
          if (IsText(S.Name))
            add(S);
        for (auto &S : Secs)
          if (!IsText(S.Name))
            add(S);
        Total = Cur - BaseVA;
      };

  // Iterate to a self-consistent layout. A section's size can shift between
  // compiles because MC relaxation depends on the (VA-derived) fixup values,
  // and pass 1 placed every section at BaseVA (overlapping), which inflates
  // sizes via spurious relaxation. Re-plan the contiguous layout and recompile
  // until the layout a compile was given reproduces itself (so every cross-
  // section fixup — e.g. AArch64 ADRP+ADD from .text into the .rodata table —
  // resolves against the address that section actually occupies).
  //
  // Section sizes are accumulated MONOTONICALLY (the max seen per section): two
  // valid layouts can otherwise relax differently and a plain "use this
  // compile's sizes" rule ping-pongs between them forever — e.g. a
  // computed-goto function whose __text relaxes to 228 then 216 bytes depending
  // on where the trailing
  // __compact_unwind/__eh_frame land, never reaching a fixed point. Spacing by
  // the max never shrinks, so once the max stops growing (bounded by the worst-
  // case relaxation) the layout is stable. The accepted compile's actual bytes
  // are <= the spacing, so each section is placed at its planned VA with zero
  // padding after it and every fixup still targets the exact VA the compile was
  // given. compileForRewrite's module mutations (triple/data-layout,
  // funnel-shift expansion, data-global externalisation) are idempotent, so
  // re-running on Mod is safe.
  for (auto &S : Res1.Sections)
    MaxSize[S.Name] = std::max<uint64_t>(MaxSize[S.Name], S.Bytes.size());
  std::map<std::string, uint64_t> SectionVA;
  uint64_t TotalSize = 0;
  planLayout(Res1.Sections, SectionVA, TotalSize);
  if (SectionVA.empty())
    return Out;

  llvm::mc_rewrite::RewriteResult Final;
  bool Converged = false;
  for (int Iter = 0; Iter < 8 && !Converged; ++Iter) {
    llvm::mc_rewrite::RewriteOptions PassN;
    PassN.Model.TextVA = BaseVA;
    PassN.Model.getSectionVA = [&](llvm::StringRef N) -> uint64_t {
      auto It = SectionVA.find(N.str());
      return It != SectionVA.end() ? It->second : BaseVA;
    };
    PassN.Model.resolve = [&](llvm::StringRef S, uint32_t Sp) {
      return ResolveFn(S, Sp);
    };
    Codegen CGn;
    auto ResN = CGn.compileForRewrite(Mod, TargetArch, PassN, Fmt);
    if (ResN.Sections.empty())
      return Out;

    // Grow the monotonic sizes from this compile, then re-plan from them.
    for (auto &S : ResN.Sections)
      MaxSize[S.Name] = std::max<uint64_t>(MaxSize[S.Name], S.Bytes.size());
    std::map<std::string, uint64_t> NewVA;
    uint64_t NewTotal = 0;
    planLayout(ResN.Sections, NewVA, NewTotal);
    if (NewVA == SectionVA) {
      // The layout this compile was given regenerates itself (the max sizes
      // have settled) → its fixups already target the final addresses. Accept
      // it; its actual section bytes (<= the max spacing) are placed at these
      // VAs.
      Final = std::move(ResN);
      TotalSize = NewTotal;
      Converged = true;
    } else {
      SectionVA = std::move(NewVA);
      TotalSize = NewTotal;
      Final = std::move(ResN);
    }
  }
  if (!Converged) {
    llvm::WithColor::error()
        << "compileImageForPatch: multi-section layout did not converge\n";
    return Out;
  }

  // Assemble the image at the converged VAs.
  Out.Bytes.assign(static_cast<size_t>(TotalSize), 0);
  for (auto &S : Final.Sections) {
    auto VAIt = SectionVA.find(S.Name);
    if (VAIt == SectionVA.end())
      continue;
    uint64_t Off = VAIt->second - BaseVA;
    if (!S.Bytes.empty()) {
      // Off/size come from the converged layout; guard the write so a layout
      // miscalculation (or a VA < BaseVA underflow) fails loudly instead of
      // corrupting memory past the assembled image.
      if (!rangeInBounds(Off, S.Bytes.size(), Out.Bytes.size())) {
        llvm::WithColor::error()
            << "compileImageForPatch: section out of image bounds\n";
        return Out;
      }
      std::memcpy(Out.Bytes.data() + Off, S.Bytes.data(), S.Bytes.size());
    }
  }

  Out.SymbolAddrs = std::move(Final.SymbolAddrs);
  Out.Unresolved = std::move(Final.Unresolved);
  Out.Success = true;
  LLVM_DEBUG(llvm::dbgs() << "compileImageForPatch: " << SectionVA.size()
                          << " sections, " << TotalSize << " bytes from VA 0x"
                          << llvm::utohexstr(BaseVA) << "\n");
  return Out;
}

//===----------------------------------------------------------------------===//
// findObjectTextSection — helper for InplaceRewriter subclasses
//===----------------------------------------------------------------------===//

bool findObjectTextSection(const std::vector<uint8_t> &Binary,
                           llvm::StringRef SectionName, TextLayout &TL) {
  if (Binary.size() < 4)
    return false;

  const uint8_t *Data = Binary.data();
  size_t Size = Binary.size();

  auto Format = llvm::identify_magic(
      llvm::StringRef(reinterpret_cast<const char *>(Data), Size));

  switch (Format) {
  case llvm::file_magic::pecoff_executable: {
    auto PE = locatePEHeaders(const_cast<uint8_t *>(Data), Size);
    if (!PE.valid())
      return false;
    PESectionFields Sec;
    if (!findPESection(PE, SectionName, Sec))
      return false;
    TL.SectionFileoff = Sec.PointerToRawData;
    TL.SectionVA = getPEImageBase(PE) + Sec.VirtualAddress;
    TL.SectionSize = Sec.VirtualSize;
    return true;
  }
  case llvm::file_magic::elf:
  case llvm::file_magic::elf_relocatable:
  case llvm::file_magic::elf_executable:
  case llvm::file_magic::elf_shared_object:
  case llvm::file_magic::elf_core: {
    ELFShdrFields Shdr;
    if (!findELFSection(Data, Size, SectionName, Shdr))
      return false;
    TL.SectionFileoff = Shdr.Offset;
    TL.SectionVA = Shdr.Addr;
    TL.SectionSize = Shdr.Size;
    return true;
  }
  case llvm::file_magic::macho_object:
  case llvm::file_magic::macho_executable:
  case llvm::file_magic::macho_dynamically_linked_shared_lib:
  case llvm::file_magic::macho_bundle: {
    uint64_t VA = 0, SecSize = 0;
    uint32_t FileOff = 0;
    if (!findMachOSection(Binary, SectionName, VA, SecSize, FileOff))
      return false;
    TL.SectionVA = VA;
    TL.SectionSize = SecSize;
    TL.SectionFileoff = FileOff;
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// BinaryPatcher common methods
//===----------------------------------------------------------------------===//

bool BinaryPatcher::writeTrampoline(std::vector<uint8_t> &Data,
                                    uint64_t FromOff, uint64_t TargetVA,
                                    uint64_t FromVA, Arch TargetArch,
                                    InstructionMode Mode,
                                    uint64_t MaxOverwriteBytes) {
  return getTargetCodegenInfo(TargetArch, Mode)
      .writeTrampoline(Data, FromOff, TargetVA, FromVA, MaxOverwriteBytes);
}

void padWithNops(uint8_t *Dst, uint64_t Len, Arch TargetArch,
                 InstructionMode Mode) {
  getTargetCodegenInfo(TargetArch, Mode).fillPadding(Dst, Len);
}

PatchResult BinaryPatcher::readPatchWrite(
    const std::filesystem::path &InputPath,
    const std::filesystem::path &OutputPath, bool SetExecPerm,
    llvm::StringRef DebugTag,
    llvm::unique_function<bool(std::vector<uint8_t> &, PatchResult &)>
        PatchFn) {
  PatchResult Result;

  auto BufOrErr = llvm::MemoryBuffer::getFile(InputPath.string());
  if (!BufOrErr) {
    llvm::WithColor::error()
        << DebugTag << ": cannot open " << InputPath.string() << "\n";
    return Result;
  }
  auto &Buf = *BufOrErr;
  std::vector<uint8_t> Binary(Buf->getBufferStart(), Buf->getBufferEnd());

  if (!PatchFn(Binary, Result))
    return Result;

  unsigned Flags = SetExecPerm ? llvm::FileOutputBuffer::F_executable : 0;
  auto OutOrErr =
      llvm::FileOutputBuffer::create(OutputPath.string(), Binary.size(), Flags);
  if (!OutOrErr) {
    llvm::WithColor::error()
        << DebugTag << ": cannot write " << OutputPath.string() << "\n";
    llvm::consumeError(OutOrErr.takeError());
    Result.Success = false;
    return Result;
  }
  std::memcpy((*OutOrErr)->getBufferStart(), Binary.data(), Binary.size());
  if (auto Err = (*OutOrErr)->commit()) {
    llvm::consumeError(std::move(Err));
    Result.Success = false;
    return Result;
  }

  Result.Success = true;
  Result.OutputPath = OutputPath.string();
  LLVM_DEBUG(llvm::dbgs() << DebugTag << ": written " << Binary.size()
                          << " bytes to " << OutputPath.string() << "\n");
  return Result;
}

static uint64_t provenFunctionSpan(
    uint64_t OrigVA, uint64_t TextStartVA, uint64_t TextEndVA,
    const std::vector<Symbol> *Symbols,
    const std::vector<std::pair<va_t, va_t>> *KnownRanges,
    const std::vector<Export> *Exports) {
  uint64_t Best = 0;
  if (Symbols) {
    for (const auto &S : *Symbols)
      if (S.IsFunc && S.Addr == OrigVA && S.Size != 0)
        Best = Best == 0 ? S.Size : std::min<uint64_t>(Best, S.Size);
  }
  if (KnownRanges) {
    for (const auto &[Begin, End] : *KnownRanges)
      if (Begin == OrigVA && End > Begin)
        Best = Best == 0 ? End - Begin : std::min<uint64_t>(Best, End - Begin);
  }
  if (Best == 0 && (Symbols || Exports)) {
    uint64_t Next = TextEndVA;
    bool FoundCurrent = false;
    bool FoundNext = false;
    if (Symbols) {
      for (const auto &S : *Symbols) {
        if (!S.IsFunc || S.Addr < TextStartVA || S.Addr >= TextEndVA)
          continue;
        FoundCurrent |= S.Addr == OrigVA;
        if (S.Addr > OrigVA) {
          Next = std::min<uint64_t>(Next, S.Addr);
          FoundNext = true;
        }
      }
    }
    if (Exports) {
      for (const auto &E : *Exports) {
        if (E.Addr < TextStartVA || E.Addr >= TextEndVA)
          continue;
        FoundCurrent |= E.Addr == OrigVA;
        if (E.Addr > OrigVA) {
          Next = std::min<uint64_t>(Next, E.Addr);
          FoundNext = true;
        }
      }
    }
    if (FoundCurrent && FoundNext && Next > OrigVA)
      Best = Next - OrigVA;
  }
  return OrigVA >= TextStartVA && OrigVA < TextEndVA
             ? std::min<uint64_t>(Best, TextEndVA - OrigVA)
             : 0;
}

size_t BinaryPatcher::installTrampolines(
    std::vector<uint8_t> &Binary,
    const std::map<std::string, uint64_t> &SymbolAddrs, uint64_t OrigTextVA,
    uint64_t OrigTextSize, uint64_t OrigTextFileOff, uint64_t ImageBase,
    Arch TargetArch, InstructionMode Mode, const std::vector<Symbol> *Symbols,
    const std::vector<std::pair<va_t, va_t>> *KnownRanges,
    const std::vector<Export> *Exports) {
  if (OrigTextSize > InvalidVA - OrigTextVA ||
      OrigTextVA > InvalidVA - ImageBase)
    return 0;
  uint64_t TextStartVA = ImageBase + OrigTextVA;
  if (OrigTextSize > InvalidVA - TextStartVA)
    return 0;
  uint64_t TextEndVA = TextStartVA + OrigTextSize;

  std::map<std::string, uint64_t> ExpMap;
  if (Exports)
    for (const auto &Exp : *Exports)
      if (Exp.Addr != 0)
        ExpMap[Exp.Name] = Exp.Addr;

  size_t Count = 0;
  for (auto &[Name, FuncNewVA] : SymbolAddrs) {
    uint64_t OrigVA = 0;
    bool HasOrig = false;

    llvm::StringRef NameRef(Name);
    if (NameRef.starts_with(kAutoFuncPrefix)) {
      llvm::StringRef HexPart = NameRef.drop_front(kAutoFuncPrefix.size());
      if (!HexPart.empty() && !HexPart.getAsInteger(16, OrigVA))
        HasOrig = true;
    }

    if (!HasOrig && !ExpMap.empty()) {
      std::string Key = resolveSymbolAlias(Name, ExpMap);
      if (!Key.empty()) {
        OrigVA = ExpMap[Key];
        HasOrig = true;
      }
    }

    if (!HasOrig)
      continue;

    uint64_t OrigRVA = OrigVA >= ImageBase ? OrigVA - ImageBase : OrigVA;
    if (OrigRVA < OrigTextVA || OrigRVA >= OrigTextVA + OrigTextSize)
      continue;

    uint64_t TextDelta = OrigRVA - OrigTextVA;
    if (OrigTextFileOff > InvalidVA - TextDelta)
      continue;
    uint64_t OrigOff = OrigTextFileOff + TextDelta;
    uint64_t OrigAnalysisVA = ImageBase + OrigRVA;
    uint64_t MaxOverwriteBytes = TextEndVA - OrigAnalysisVA;
    if (TargetArch == Arch::ARM && Mode == InstructionMode::Thumb)
      MaxOverwriteBytes = provenFunctionSpan(OrigAnalysisVA, TextStartVA,
                                             TextEndVA, Symbols, KnownRanges,
                                             Exports);
    if (writeTrampoline(Binary, OrigOff, FuncNewVA, ImageBase + OrigRVA,
                        TargetArch, Mode, MaxOverwriteBytes)) {
      ++Count;
      LLVM_DEBUG(llvm::dbgs() << "patch: trampoline " << Name << " @ VA 0x"
                              << llvm::utohexstr(OrigRVA) << " -> 0x"
                              << llvm::utohexstr(FuncNewVA) << "\n");
    } else {
      LLVM_DEBUG(llvm::dbgs()
                 << "patch: skipped unsafe/unreachable trampoline " << Name
                 << " @ VA 0x" << llvm::utohexstr(OrigAnalysisVA) << "\n");
    }
  }
  return Count;
}

} // namespace neverd
