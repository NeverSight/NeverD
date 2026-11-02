//===- InplaceRewriter.cpp - In-place binary rewriting logic -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements format-agnostic in-place binary rewriting operations shared
/// by the COFF, ELF, and Mach-O inplace pipelines.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/support/TargetCodegenInfo.h"

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
  patch_receipt_detail::publishCommitted(
      Result, State.Mappings,
      [](const InplaceMapping &Mapping) { return Mapping.OrigVA; });
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
  if (!isSingleInstructionMode(Image.Mode) &&
      !(Image.Mode == InstructionMode::MixedARMThumb &&
        TargetArch == Arch::ARM && Image.Format == BinaryFormat::ELF)) {
    llvm::WithColor::error()
        << "inplace: rewriting requires a single instruction mode (got "
        << getInstructionModeName(Image.Mode) << ")\n";
    return PatchResult{};
  }

  const BinaryFormat Fmt = getBinaryFormat();
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
    if (TargetArch == Arch::X86 && RequireGeneratedEHContinuations) {
      // Registration callbacks and their localrecover frame are one module
      // contract. Use its authenticated source VAs before name-only matching
      // or per-function probes can discard that identity or its helpers.
      COFFPatcher Patcher;
      Patcher.setImageContext(&Image);
      Patcher.setTextSectionOverride(TextSectionOverride);
      return Patcher.patch(InputPath, OutputPath, Mod, TargetArch);
    }
  }

  auto BufOrErr = llvm::MemoryBuffer::getFile(InputPath.string());
  if (!BufOrErr) {
    llvm::WithColor::error()
        << "inplace: cannot open " << InputPath.string() << "\n";
    return PatchResult{};
  }

  RewriteState State;
  State.Binary.assign((*BufOrErr)->getBufferStart(),
                      (*BufOrErr)->getBufferEnd());
  if (Fmt == BinaryFormat::MachO) {
    // Authenticate the exact snapshot being rewritten, not the caller's
    // subtype metadata or a module annotation supplied by the lifting pipeline.
    auto Loaded = MachOLoader().loadBuffer(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(State.Binary.data()),
                        State.Binary.size()),
        (*BufOrErr)->getBufferIdentifier()));
    if (!Loaded) {
      llvm::WithColor::error() << llvm::toString(Loaded.takeError()) << '\n';
      return PatchResult{};
    }
    if (Loaded->isArm64e()) {
      llvm::WithColor::error() << kArm64eRebuildUnsupported;
      return PatchResult{};
    }
  }
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
    InstructionMode Mode = InstructionMode::Default;
  };
  std::vector<FuncPlan> Plans;

  for (auto &F : Mod) {
    if (F.isDeclaration())
      continue;
    std::string Resolved = resolveSymbolAlias(F.getName().str(), SymByName);
    if (Resolved.empty() && Image.isELF() && Image.Entry != 0) {
      // A stripped ELF entry point, or one marked STT_NOTYPE, has no function
      // symbol to match.  Its exact lifted source identity and the loader's
      // authenticated entry are sufficient to recover this one replacement.
      auto OriginalVA = rewrite_source::getOriginalVA(F);
      if (!OriginalVA) {
        llvm::WithColor::error()
            << "inplace: " << llvm::toString(OriginalVA.takeError()) << "\n";
        return PatchResult{};
      }
      if (*OriginalVA && **OriginalVA == Image.Entry &&
          Image.hasAuthenticatedFunctionEntryAt(Image.Entry)) {
        Symbol EntrySymbol = Symbol::makeFunc(Image.Entry);
        EntrySymbol.Name = F.getName().str();
        SymByName.emplace(EntrySymbol.Name, std::move(EntrySymbol));
        Resolved = F.getName().str();
      }
    }
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
      if (const va_t Next = Image.nextKnownFunctionEntryAfter(OrigVA);
          Next > OrigVA && Next < Closest)
        Closest = Next;
      OrigSize = Closest - OrigVA;
    }

    FuncPlan P;
    P.Name = Resolved;
    P.IRName = F.getName().str();
    P.OrigVA = OrigVA;
    P.OrigSize = OrigSize;
    P.HasExceptionMetadata =
        Image.ExceptionMetadata.findFunction(OrigVA) != nullptr;
    if (Image.Mode == InstructionMode::MixedARMThumb) {
      const auto Mode = Image.instructionModeAt(OrigVA);
      if (!Mode) {
        llvm::WithColor::error()
            << "inplace: function has no authenticated ARM mode\n";
        return PatchResult{};
      }
      P.Mode = *Mode;
    } else {
      P.Mode = Image.Mode;
    }
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
  for (auto &P : Plans) {
    FuncOrigVAs[P.IRName] = P.OrigVA;
    if (Image.Mode != InstructionMode::MixedARMThumb)
      continue;
    llvm::Function *Function = Mod.getFunction(P.IRName);
    if (!Function)
      return PatchResult{};
    Function->addFnAttr("disable-tail-calls", "true");
    const llvm::StringRef Wanted =
        P.Mode == InstructionMode::Thumb ? "+thumb-mode" : "-thumb-mode";
    const llvm::StringRef Conflict =
        P.Mode == InstructionMode::Thumb ? "-thumb-mode" : "+thumb-mode";
    const llvm::Attribute Existing =
        Function->getFnAttribute("target-features");
    std::string Features =
        Existing.isValid() ? Existing.getValueAsString().str() : std::string();
    if (llvm::StringRef(Features).contains(Conflict)) {
      llvm::WithColor::error()
          << "inplace: function target features contradict ARM mode\n";
      return PatchResult{};
    }
    if (!llvm::StringRef(Features).contains(Wanted)) {
      if (!Features.empty())
        Features += ',';
      Features += Wanted.str();
      Function->addFnAttr("target-features", Features);
    }
  }

  auto Resolver = createRelocResolver();
  if (!Resolver->populateFromImage(Image, TargetArch))
    Resolver->parse(State.Binary, TargetArch);

  if (Fmt == BinaryFormat::MachO && TargetArch == Arch::ARM) {
    llvm::WithColor::error()
        << "inplace: ARM Mach-O requires a function-level ABI target; use "
           "the transactional section patch path\n";
    return PatchResult{};
  }
  auto SerializeResolvedCode = [&](uint64_t VA,
                                   bool IsCode) -> std::optional<uint64_t> {
    if (!IsCode)
      return VA;
    const auto Mode = Image.instructionModeAt(VA);
    return Mode ? std::optional<uint64_t>(
                      serializeCodePointer(VA, TargetArch, *Mode))
                : std::nullopt;
  };
  auto IsExecutable = [&](uint64_t VA) { return Image.isCodeAddress(VA); };
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

    auto ResolveFn = [&](llvm::StringRef Sym,
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
        return serializeImageDataSymbolAddress(Image, *Parsed);
      if (auto Parsed = parseNdCodePtrSymbol(Name))
        return SerializeResolvedCode(*Parsed, false);
      std::string AliasKey = resolveSymbolAlias(Name, SymByName);
      if (!AliasKey.empty())
        return SerializeResolvedCode(SymByName[AliasKey].Addr, true);
      if (Image.isELF() && isSynthesizedFuncName(Name)) {
        llvm::StringRef Spelling(Name);
        if (Spelling.consume_front(kAutoFuncPrefix)) {
          va_t Address = 0;
          if (!Spelling.getAsInteger(16, Address) &&
              Image.hasAuthenticatedFunctionEntryAt(Address))
            return SerializeResolvedCode(Address, true);
        }
      }
      return std::nullopt;
    };

    // Probe with the same placement-aware compiler used by the grower path.
    // Anchoring every emitted section at OrigVA is invalid when a function
    // entry is only instruction-aligned but a literal/data section requires
    // 16-byte alignment (AArch64 then rejects scale16 fixups before we can
    // discover that the function must be relocated).  The compiled-image
    // planner keeps text at OrigVA, lays out every extra section at its native
    // alignment, and returns the exact text slice needed for the fit/grow
    // decision.
    auto ProbeImage = compileImageForPatch(*ClonedMod, TargetArch, Fmt,
                                           Plan.OrigVA, ResolveFn, Image.Base);
    if (!ProbeImage.FunctionRangesValid) {
      llvm::WithColor::error()
          << "inplace: rewrite image provenance is invalid\n";
      return PatchResult{};
    }
    if (!ProbeImage.Success) {
      llvm::WithColor::error() << "inplace: rewrite image layout is invalid\n";
      return PatchResult{};
    }
    std::string InterworkingDetail;
    if (!repairMixedARMInterworkingCalls(ProbeImage, Image,
                                         InterworkingDetail)) {
      llvm::WithColor::error()
          << "inplace: ARM interworking validation failed: "
          << InterworkingDetail << "\n";
      return PatchResult{};
    }
    if (ProbeImage.Sections.empty())
      continue;

    const CompiledSection *TextSec = nullptr;
    for (auto &S : ProbeImage.Sections) {
      if (section_names::isTextSectionName(S.Name)) {
        TextSec = &S;
        break;
      }
    }
    if (!TextSec || TextSec->Size == 0) {
      if (!ProbeImage.Sections.empty())
        TextSec = &ProbeImage.Sections[0];
    }
    if (TextSec && TextSec->IsInImage && TextSec->Size > 0 &&
        rangeInBounds(TextSec->Offset, TextSec->Size,
                      ProbeImage.Bytes.size())) {
      const auto Begin = ProbeImage.Bytes.begin() + TextSec->Offset;
      Plan.NewBytes.assign(Begin, Begin + TextSec->Size);
    }

    // Any non-text section with content (e.g. an indirect-branch offset table)
    // cannot be placed by the in-place overwrite path, which keeps only .text.
    // Force such functions through the relocating path (compileImageForPatch).
    for (auto &S : ProbeImage.Sections)
      if (&S != TextSec && S.Size > 0)
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

    const bool NeedsFormatAwareExceptionPatch =
        Fmt != BinaryFormat::COFF &&
        std::any_of(Plans.begin(), Plans.end(), [&](const FuncPlan &Plan) {
          return Plan.HasExceptionMetadata && GrowNames.count(Plan.IRName);
        });
    if (NeedsFormatAwareExceptionPatch) {
      if (!Patcher) {
        llvm::WithColor::error()
            << "inplace: exception-aware patching is unsupported for this "
               "format\n";
        return PatchResult{};
      }
      Patcher->setImageContext(&Image);
      return Patcher->patch(InputPath, OutputPath, Mod, TargetArch);
    }

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
        return serializeImageDataSymbolAddress(Image, *Parsed);
      if (auto Parsed = parseNdCodePtrSymbol(Name))
        return SerializeResolvedCode(*Parsed, false);
      std::string AliasKey = resolveSymbolAlias(Name, SymByName);
      if (!AliasKey.empty())
        return SerializeResolvedCode(SymByName[AliasKey].Addr, true);
      if (Image.isELF() && isSynthesizedFuncName(Name)) {
        llvm::StringRef Spelling(Name);
        if (Spelling.consume_front(kAutoFuncPrefix)) {
          va_t Address = 0;
          if (!Spelling.getAsInteger(16, Address) &&
              Image.hasAuthenticatedFunctionEntryAt(Address))
            return SerializeResolvedCode(Address, true);
        }
      }
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
    std::string InterworkingDetail;
    if (!repairMixedARMInterworkingCalls(ImageOut, Image, InterworkingDetail)) {
      llvm::WithColor::error()
          << "inplace: ARM interworking validation failed: "
          << InterworkingDetail << "\n";
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
                    Plan.OrigSize - CopySize, TargetArch, Plan.Mode);
      RecordApplied(Plan);
    }

    // Install a trampoline at each grower's original VA -> its new VA.
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
        llvm::WithColor::error() << "inplace: file offset overflow for grower '"
                                 << Plan.Name << "'\n";
        return PatchResult{};
      }
      uint64_t FileOff = State.TL.SectionFileoff + TextDelta;
      const auto TCI = getTargetCodegenInfo(TargetArch, Plan.Mode);
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
          PatchedEntryMappings, NewSegVA, TargetArch, GrowMod.get());
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
      if (llvm::Error Err = validatePatchedCOFFImage(
              State.Binary, TargetArch, EHUpdate.Apply && EHUpdate.Size != 0,
              EHUpdate)) {
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
                  Plan.OrigSize - CopySize, TargetArch, Plan.Mode);

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

} // namespace neverd
