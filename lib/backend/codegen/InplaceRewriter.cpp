//===- InplaceRewriter.cpp - In-place binary rewriting logic -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements format-agnostic in-place binary rewriting operations shared
/// by the COFF, ELF, and Mach-O inplace pipelines.  Also contains the
/// shared RelocResolver methods.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/TargetCodegenInfo.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#define DEBUG_TYPE "neverd-rewriter"

namespace neverd {

// ===--------------------------------------------------------------------===//
// InplaceRewriter default virtual implementations
// ===--------------------------------------------------------------------===//

bool InplaceRewriter::parseTextSection(const std::vector<uint8_t> &Binary,
                                       const BinaryImage &Image,
                                       TextLayout &TL) {
  // A user-forced section name (e.g. "--text-section .vmp0") wins: it is the
  // caller's explicit statement of where the original code lives, so it is
  // tried before the format default and before the flag-based fallback.
  if (!TextSectionOverride.empty() &&
      findObjectTextSection(Binary, TextSectionOverride, TL))
    return true;

  llvm::StringRef Name = getTextSectionName();
  if (findObjectTextSection(Binary, Name, TL))
    return true;

  // The canonical code section name is absent. This is the normal state for an
  // input that was already processed by a packer/protector, which renames its
  // code section (VMProtect ".vmp0", UPX "UPX1", Themida, randomised names).
  // Fall back to the loader's flag-based pick (executable section containing
  // the entry point, else the largest executable section). The layout is
  // sourced from the same BinaryImage as the function symbols, so the
  // VA/size/offset space stays self-consistent with the rest of rewrite().
  if (const Section *Text = Image.getTextSection()) {
    TL.SectionVA = Text->VA;
    TL.SectionSize = Text->Size;
    TL.SectionFileoff = Text->FileOff;
    llvm::WithColor::warning()
        << "inplace: " << Name << " not found; hardening executable section '"
        << Text->Name << "'\n";
    return true;
  }

  llvm::WithColor::error() << "inplace: " << Name << " section not found\n";
  return false;
}

PatchResult
InplaceRewriter::writeResult(const std::filesystem::path &OutputPath,
                             const RewriteState &State, bool SetExecPerm) {
  PatchResult Result;

  unsigned Flags = SetExecPerm ? llvm::FileOutputBuffer::F_executable : 0;
  auto OutOrErr = llvm::FileOutputBuffer::create(OutputPath.string(),
                                                 State.Binary.size(), Flags);
  if (!OutOrErr) {
    llvm::WithColor::error()
        << "inplace: cannot write " << OutputPath.string() << "\n";
    llvm::consumeError(OutOrErr.takeError());
    return Result;
  }
  std::memcpy((*OutOrErr)->getBufferStart(), State.Binary.data(),
              State.Binary.size());
  if (auto Err = (*OutOrErr)->commit()) {
    llvm::consumeError(std::move(Err));
    return Result;
  }

  Result.Success = true;
  Result.OutputPath = OutputPath.string();
  Result.CodeSize = State.ObjText.size();
  Result.TrampolineCount = State.TrampolineCount;
  return Result;
}

// ===--------------------------------------------------------------------===//
// InplaceRewriter — per-function compilation
// ===--------------------------------------------------------------------===//

PatchResult InplaceRewriter::rewrite(const std::filesystem::path &InputPath,
                                     const std::filesystem::path &OutputPath,
                                     llvm::Module &Mod,
                                     const BinaryImage &Image,
                                     Arch TargetArch) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(InputPath.string());
  if (!BufOrErr) {
    llvm::WithColor::error()
        << "inplace: cannot open " << InputPath.string() << "\n";
    return PatchResult{};
  }

  RewriteState State;
  State.Binary.assign((*BufOrErr)->getBufferStart(),
                      (*BufOrErr)->getBufferEnd());
  if (!parseTextSection(State.Binary, Image, State.TL))
    return PatchResult{};

  std::map<std::string, Symbol> SymByName;
  for (auto &Sym : Image.Symbols)
    if (isValidFunctionSymbol(Image, Sym))
      SymByName[Sym.Name] = Sym;

  struct FuncPlan {
    std::string Name;
    std::string IRName;
    uint64_t OrigVA = 0;
    uint64_t OrigSize = 0;
    std::vector<uint8_t> NewBytes;
    // True when the recompiled function emits a non-text section (e.g. the
    // indirect-branch pass's PIC offset table in __const/.rodata).  Such a
    // function cannot be patched in place — the data section must be placed
    // too — so it is forced through the relocating (grower) path, which uses
    // compileImageForPatch for proper multi-section layout.
    bool HasExtraSections = false;
    /// Any existing table-based unwind contract forces relocation so the
    /// original runtime entry can be replaced atomically with codegen's new
    /// `.pdata/.xdata` rather than leaving stale prologue metadata in place.
    bool HasExceptionMetadata = false;
  };
  std::vector<FuncPlan> Plans;

  for (auto &F : Mod) {
    if (F.isDeclaration())
      continue;
    std::string Resolved = resolveSymbolAlias(F.getName().str(), SymByName);
    if (Resolved.empty())
      continue;
    auto It = SymByName.find(Resolved);
    uint64_t OrigVA = It->second.Addr;
    if (OrigVA < State.TL.SectionVA ||
        OrigVA >= State.TL.SectionVA + State.TL.SectionSize)
      continue;

    uint64_t OrigSize = It->second.Size;
    if (OrigSize == 0) {
      uint64_t SectionEnd = State.TL.SectionVA + State.TL.SectionSize;
      uint64_t Closest = SectionEnd;
      for (auto &[_, S] : SymByName)
        if (S.Addr > OrigVA && S.Addr < Closest && S.IsFunc)
          Closest = S.Addr;
      OrigSize = Closest - OrigVA;
    }

    FuncPlan P;
    P.Name = Resolved;
    P.IRName = F.getName().str();
    P.OrigVA = OrigVA;
    P.OrigSize = OrigSize;
    P.HasExceptionMetadata =
        Image.ExceptionMetadata.findFunction(OrigVA) != nullptr;
    Plans.push_back(std::move(P));
  }

  if (Plans.empty()) {
    llvm::WithColor::error()
        << "inplace: no matching functions for replacement\n";
    return PatchResult{};
  }

  std::sort(
      Plans.begin(), Plans.end(),
      [](const FuncPlan &A, const FuncPlan &B) { return A.OrigVA < B.OrigVA; });

  std::map<std::string, uint64_t> FuncOrigVAs;
  for (auto &P : Plans)
    FuncOrigVAs[P.IRName] = P.OrigVA;

  auto Resolver = createRelocResolver();
  if (!Resolver->populateFromImage(Image, TargetArch))
    Resolver->parse(State.Binary, TargetArch);

  BinaryFormat Fmt = getBinaryFormat();
  bool RequireGeneratedEHContinuations = false;
  if (Fmt == BinaryFormat::COFF) {
    auto EHPlanOrErr = planCOFFExceptionPatch(Mod, Image, TargetArch);
    if (!EHPlanOrErr) {
      llvm::WithColor::error()
          << llvm::toString(EHPlanOrErr.takeError()) << "\n";
      return PatchResult{};
    }
    RequireGeneratedEHContinuations =
        !EHPlanOrErr->LanguageExceptionFunctionEntries.empty();
  }
  InstructionMode ResolveMode = Image.Mode;
  auto SerializeResolvedCode = [&](uint64_t VA, bool IsCode) {
    return IsCode ? serializeCodePointer(VA, TargetArch, ResolveMode) : VA;
  };
  auto IsExecutable = [&](uint64_t VA) {
    const Segment *Seg = Image.getSegmentFor(VA);
    return Seg && Seg->isExecutable();
  };
  auto IsProvenCodeDataSymbol = [&](uint64_t VA) {
    return IsExecutable(VA) && Image.CodeRefTargets.count(VA) != 0;
  };
  std::map<std::string, uint64_t> ExportAddrs;
  for (const auto &E : Image.Exports)
    if (!E.Name.empty())
      ExportAddrs[E.Name] = E.Addr;

  for (auto &Plan : Plans) {
    auto ClonedMod = llvm::CloneModule(Mod);

    for (auto &F : *ClonedMod) {
      if (F.isDeclaration())
        continue;
      if (F.getName() != Plan.IRName)
        F.deleteBody();
    }

    llvm::mc_rewrite::RewriteOptions RwOpts;
    RwOpts.Model.TextVA = Plan.OrigVA;
    RwOpts.Model.ImageBaseVA = Image.Base;
    RwOpts.Model.getSectionVA = [&](llvm::StringRef) -> uint64_t {
      return Plan.OrigVA;
    };
    RwOpts.Model.resolve = [&](llvm::StringRef Sym,
                               uint32_t) -> std::optional<uint64_t> {
      std::string Name = Sym.str();
      auto FIt = FuncOrigVAs.find(Name);
      if (FIt != FuncOrigVAs.end())
        return SerializeResolvedCode(FIt->second, true);
      if (const auto *Entry = Resolver->findEntry(Name))
        return SerializeResolvedCode(Entry->Addr, Entry->IsCode);
      std::string ExportKey = resolveSymbolAlias(Name, ExportAddrs);
      if (!ExportKey.empty()) {
        uint64_t VA = ExportAddrs.at(ExportKey);
        return serializeExportAddress(Image, VA);
      }
      if (auto Parsed = parseNdDataSymbol(Name))
        return SerializeResolvedCode(*Parsed,
                                     IsProvenCodeDataSymbol(*Parsed));
      if (auto Parsed = parseNdCodePtrSymbol(Name))
        return SerializeResolvedCode(*Parsed, false);
      std::string AliasKey = resolveSymbolAlias(Name, SymByName);
      if (!AliasKey.empty())
        return SerializeResolvedCode(SymByName[AliasKey].Addr, true);
      return std::nullopt;
    };

    Codegen CG;
    auto RwResult = CG.compileForRewrite(*ClonedMod, TargetArch, RwOpts, Fmt);
    if (RwResult.Sections.empty())
      continue;

    const llvm::mc_rewrite::RewriteSection *TextSec = nullptr;
    for (auto &S : RwResult.Sections) {
      if (section_names::isTextSectionName(S.Name)) {
        TextSec = &S;
        break;
      }
    }
    if (!TextSec || TextSec->Bytes.empty()) {
      if (!RwResult.Sections.empty())
        TextSec = &RwResult.Sections[0];
    }
    if (TextSec && !TextSec->Bytes.empty())
      Plan.NewBytes = TextSec->Bytes;

    // Any non-text section with content (e.g. an indirect-branch offset table)
    // cannot be placed by the in-place overwrite path, which keeps only .text.
    // Force such functions through the relocating path (compileImageForPatch).
    for (auto &S : RwResult.Sections)
      if (&S != TextSec && !S.Bytes.empty())
        Plan.HasExtraSections = true;
  }

  // In-place overwrite cannot enlarge a function: a recompiled body bigger
  // than its original slot would be truncated, corrupting the code. Growth is
  // common once a function gains data / PC-relative references. Per-function
  // relocation handles this without forcing the whole module out of place:
  // functions that still fit are overwritten in place; growers are recompiled
  // together into a fresh executable segment (placed at plannedExecSegmentVA)
  // and a trampoline is installed at each grower's original VA. Callers into a
  // grower (whether a fit function still living at the original VA, or the
  // program entry) reach it through that trampoline; growers call fit
  // functions directly at their unchanged VAs, and call each other in-section.
  std::set<std::string> GrowNames;
  for (const auto &Plan : Plans)
    if (Plan.NewBytes.size() > Plan.OrigSize || Plan.HasExtraSections ||
        Plan.HasExceptionMetadata)
      GrowNames.insert(Plan.IRName);

  if (!GrowNames.empty()) {
    auto Patcher = createBinaryPatcher();
    uint64_t NewSegVA =
        Patcher ? Patcher->plannedExecSegmentVA(State.Binary, TargetArch) : 0;

    // Formats without segment-append support fall back to whole-module section
    // patching (relocates every body, trampolines all original VAs).
    if (!Patcher || NewSegVA == 0) {
      if (!Patcher) {
        llvm::WithColor::error()
            << "inplace: function growth needs section fallback, "
               "unsupported for this format\n";
        return PatchResult{};
      }
      Patcher->setImageContext(&Image);
      return Patcher->patch(InputPath, OutputPath, Mod, TargetArch);
    }
    Patcher->setImageContext(&Image);

    // Compile just the growers together, laid out at the new segment VA.
    auto GrowMod = llvm::CloneModule(Mod);
    for (auto &F : *GrowMod)
      if (!F.isDeclaration() && !GrowNames.count(F.getName().str()))
        F.deleteBody();

    auto ResolveFn = [&](llvm::StringRef Sym,
                         uint32_t) -> std::optional<uint64_t> {
      std::string Name = Sym.str();
      if (Fmt == BinaryFormat::COFF)
        if (auto Personality = findCOFFExceptionPersonalityVA(Image, Sym))
          return SerializeResolvedCode(*Personality, true);
      auto FIt = FuncOrigVAs.find(Name);
      if (FIt != FuncOrigVAs.end())
        return SerializeResolvedCode(FIt->second, true);
      if (const auto *Entry = Resolver->findEntry(Name))
        return SerializeResolvedCode(Entry->Addr, Entry->IsCode);
      std::string ExportKey = resolveSymbolAlias(Name, ExportAddrs);
      if (!ExportKey.empty()) {
        uint64_t VA = ExportAddrs.at(ExportKey);
        return SerializeResolvedCode(VA, IsExecutable(VA));
      }
      if (auto Parsed = parseNdDataSymbol(Name))
        return SerializeResolvedCode(*Parsed,
                                     IsProvenCodeDataSymbol(*Parsed));
      if (auto Parsed = parseNdCodePtrSymbol(Name))
        return SerializeResolvedCode(*Parsed, false);
      std::string AliasKey = resolveSymbolAlias(Name, SymByName);
      if (!AliasKey.empty())
        return SerializeResolvedCode(SymByName[AliasKey].Addr, true);
      return std::nullopt;
    };

    // compileImageForPatch lays out text + any non-text sections (e.g. the
    // indirect-branch offset table) contiguously from NewSegVA and resolves all
    // cross-section fixups, so a grower that emits a data table works too.  For
    // text-only growers it is a single-section fast path identical to before.
    auto ImageOut = compileImageForPatch(*GrowMod, TargetArch, Fmt, NewSegVA,
                                         ResolveFn, Image.Base);
    if (!ImageOut.Success || ImageOut.Bytes.empty()) {
      llvm::WithColor::error()
          << "inplace: grower recompile produced no code\n";
      return PatchResult{};
    }

    std::vector<InplaceMapping> AppliedMappings;
    auto RecordApplied = [&](const auto &Plan) {
      InplaceMapping M;
      M.Name = Plan.Name;
      M.OrigVA = Plan.OrigVA;
      M.OrigSize = Plan.OrigSize;
      M.NewSize = Plan.NewBytes.size();
      M.Shift = 0;
      AppliedMappings.push_back(std::move(M));
    };

    // Overwrite the functions that still fit, in place.
    for (auto &Plan : Plans) {
      if (GrowNames.count(Plan.IRName) || Plan.NewBytes.empty())
        continue;
      uint64_t CopySize =
          std::min<uint64_t>(Plan.NewBytes.size(), Plan.OrigSize);
      uint64_t TextDelta = Plan.OrigVA - State.TL.SectionVA;
      if (State.TL.SectionFileoff > InvalidVA - TextDelta) {
        llvm::WithColor::error()
            << "inplace: file offset overflow for replacement '" << Plan.Name
            << "'\n";
        return PatchResult{};
      }
      uint64_t FileOff = State.TL.SectionFileoff + TextDelta;
      // Overflow-safe: a corrupt symbol size can make FileOff + OrigSize wrap,
      // which would bypass the check and let padWithNops() write out of bounds.
      if (FileOff > State.Binary.size() ||
          Plan.OrigSize > State.Binary.size() - FileOff) {
        llvm::WithColor::error()
            << "inplace: replacement slot for '" << Plan.Name
            << "' is outside the input image\n";
        return PatchResult{};
      }
      std::memcpy(State.Binary.data() + FileOff, Plan.NewBytes.data(),
                  CopySize);
      if (CopySize < Plan.OrigSize)
        padWithNops(State.Binary.data() + FileOff + CopySize,
                    Plan.OrigSize - CopySize, TargetArch, Image.Mode);
      RecordApplied(Plan);
    }

    // Install a trampoline at each grower's original VA -> its new VA.
    auto TCI = getTargetCodegenInfo(TargetArch, Image.Mode);
    auto findNewVA = [&](const std::string &IRName) -> uint64_t {
      for (const std::string &Cand :
           {IRName, "_" + IRName,
            (!IRName.empty() && IRName[0] == '_') ? IRName.substr(1)
                                                  : IRName}) {
        auto It = ImageOut.SymbolAddrs.find(Cand);
        if (It != ImageOut.SymbolAddrs.end())
          return It->second;
      }
      return 0;
    };

    size_t InstalledTrampolines = 0;
    std::vector<va_t> PatchedOriginalEntries;
    std::vector<std::pair<va_t, va_t>> PatchedEntryMappings;
    for (auto &Plan : Plans) {
      if (!GrowNames.count(Plan.IRName))
        continue;
      uint64_t NewVA = findNewVA(Plan.IRName);
      if (NewVA == 0) {
        llvm::WithColor::error() << "inplace: grower '" << Plan.Name
                                 << "' missing from relocated segment\n";
        return PatchResult{};
      }
      uint64_t TextDelta = Plan.OrigVA - State.TL.SectionVA;
      if (State.TL.SectionFileoff > InvalidVA - TextDelta) {
        llvm::WithColor::error()
            << "inplace: file offset overflow for grower '" << Plan.Name
            << "'\n";
        return PatchResult{};
      }
      uint64_t FileOff = State.TL.SectionFileoff + TextDelta;
      uint64_t TrampolineSize = TCI.trampolineSize();
      if (TrampolineSize == 0 || FileOff > State.Binary.size() ||
          State.Binary.size() - FileOff < TrampolineSize) {
        llvm::WithColor::error()
            << "inplace: trampoline slot for grower '" << Plan.Name
            << "' is outside the input image\n";
        return PatchResult{};
      }
      if (!TCI.writeTrampoline(State.Binary, FileOff, NewVA, Plan.OrigVA,
                               Plan.OrigSize)) {
        llvm::WithColor::error()
            << "inplace: cannot install safe trampoline for grower '"
            << Plan.Name << "' (span=" << Plan.OrigSize
            << ", required=" << TrampolineSize << ")\n";
        return PatchResult{};
      }
      ++InstalledTrampolines;
      PatchedOriginalEntries.push_back(Plan.OrigVA);
      PatchedEntryMappings.push_back({Plan.OrigVA, NewVA});
      RecordApplied(Plan);
      LLVM_DEBUG(llvm::dbgs() << "inplace: relocated grower '" << Plan.Name
                              << "' VA=0x" << llvm::utohexstr(Plan.OrigVA)
                              << " -> 0x" << llvm::utohexstr(NewVA) << "\n");
    }

    COFFExceptionDirectoryUpdate EHUpdate;
    COFFGuardTableUpdate GuardUpdate;
    if (Fmt == BinaryFormat::COFF) {
      auto EHUpdateOrErr = prepareCOFFExceptionDirectory(
          State.Binary, Image, ImageOut, PatchedOriginalEntries,
          PatchedEntryMappings, NewSegVA, TargetArch);
      if (!EHUpdateOrErr) {
        llvm::WithColor::error()
            << llvm::toString(EHUpdateOrErr.takeError()) << "\n";
        return PatchResult{};
      }
      EHUpdate = *EHUpdateOrErr;
      auto GuardUpdateOrErr = prepareCOFFGuardTables(
          State.Binary, Image, ImageOut, PatchedEntryMappings, NewSegVA,
          TargetArch, RequireGeneratedEHContinuations);
      if (!GuardUpdateOrErr) {
        llvm::WithColor::error()
            << llvm::toString(GuardUpdateOrErr.takeError()) << "\n";
        return PatchResult{};
      }
      GuardUpdate = *GuardUpdateOrErr;
    }

    uint64_t Placed = Patcher->appendExecSegment(State.Binary, ImageOut.Bytes,
                                                 "", TargetArch);
    if (Placed == 0 || Placed != NewSegVA) {
      llvm::WithColor::error() << "inplace: appendExecSegment failed\n";
      return PatchResult{};
    }
    if (Fmt == BinaryFormat::COFF) {
      if (llvm::Error Err =
              applyCOFFExceptionDirectoryUpdate(State.Binary, EHUpdate)) {
        llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
        return PatchResult{};
      }
      if (llvm::Error Err =
              applyCOFFGuardTableUpdate(State.Binary, Image, GuardUpdate)) {
        llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
        return PatchResult{};
      }
      if (llvm::Error Err =
              validatePatchedCOFFImage(State.Binary, TargetArch)) {
        llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
        return PatchResult{};
      }
    }

    if (!ImageOut.Unresolved.empty())
      llvm::WithColor::warning() << "inplace: " << ImageOut.Unresolved.size()
                                 << " unresolved symbols\n";

    State.ObjText = ImageOut.Bytes;
    State.Mappings = std::move(AppliedMappings);
    State.TrampolineCount = InstalledTrampolines;
    return writeResult(OutputPath, State, needsExecPermission());
  }

  State.Mappings.clear();
  for (auto &Plan : Plans) {
    if (Plan.NewBytes.empty())
      continue;
    uint64_t CopySize = Plan.NewBytes.size();
    if (CopySize > Plan.OrigSize)
      CopySize = Plan.OrigSize;

    uint64_t FileOff =
        State.TL.SectionFileoff + (Plan.OrigVA - State.TL.SectionVA);
    // Overflow-safe: a corrupt symbol size can make FileOff + OrigSize wrap,
    // which would bypass the check and let padWithNops() write out of bounds.
    if (FileOff > State.Binary.size() ||
        Plan.OrigSize > State.Binary.size() - FileOff)
      continue;

    std::memcpy(State.Binary.data() + FileOff, Plan.NewBytes.data(), CopySize);
    if (CopySize < Plan.OrigSize)
      padWithNops(State.Binary.data() + FileOff + CopySize,
                  Plan.OrigSize - CopySize, TargetArch, Image.Mode);

    LLVM_DEBUG(llvm::dbgs()
               << "inplace: replaced '" << Plan.Name << "' at VA=0x"
               << llvm::utohexstr(Plan.OrigVA) << " (orig=" << Plan.OrigSize
               << ", new=" << Plan.NewBytes.size() << ")\n");

    InplaceMapping M;
    M.Name = Plan.Name;
    M.OrigVA = Plan.OrigVA;
    M.OrigSize = Plan.OrigSize;
    M.NewSize = Plan.NewBytes.size();
    M.Shift = 0;
    State.Mappings.push_back(std::move(M));
  }

  State.ObjText.clear();

  if (Fmt == BinaryFormat::COFF) {
    if (llvm::Error Err = validatePatchedCOFFImage(State.Binary, TargetArch)) {
      llvm::WithColor::error() << llvm::toString(std::move(Err)) << "\n";
      return PatchResult{};
    }
  }

  return writeResult(OutputPath, State, needsExecPermission());
}

// ===--------------------------------------------------------------------===//
// RelocResolver common methods
// ===--------------------------------------------------------------------===//

bool RelocResolver::populateFromImage(const BinaryImage &Image, Arch) {
  Entries.clear();
  ByName.clear();

  for (const auto &Imp : Image.Imports) {
    if (Imp.Name.empty() || Imp.IATAddr == 0)
      continue;
    if (ByName.count(Imp.Name))
      continue;
    RelocEntry E;
    E.Name = Imp.Name;
    E.Addr = Imp.IATAddr;
    E.IsCode = false;
    ByName[E.Name] = Entries.size();
    Entries.push_back(std::move(E));
  }

  for (const auto &Sym : Image.Symbols) {
    if (!isValidFunctionSymbol(Image, Sym) || Sym.Name.empty())
      continue;
    if (ByName.count(Sym.Name))
      continue;
    RelocEntry E;
    E.Name = Sym.Name;
    E.Addr = Sym.Addr;
    E.IsCode = true;
    ByName[E.Name] = Entries.size();
    Entries.push_back(std::move(E));
  }

  LLVM_DEBUG(llvm::dbgs() << "reloc: populated " << Entries.size()
                          << " entries from image\n");
  return !Entries.empty();
}

va_t RelocResolver::findSymbol(const std::string &Symbol) const {
  const RelocEntry *Entry = findEntry(Symbol);
  return Entry ? Entry->Addr : InvalidVA;
}

const RelocResolver::RelocEntry *
RelocResolver::findEntry(const std::string &Symbol) const {
  std::string Key = resolveSymbolAlias(Symbol, ByName);
  if (!Key.empty())
    return &Entries[ByName.at(Key)];
  return nullptr;
}

} // namespace neverd
