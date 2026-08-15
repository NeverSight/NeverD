//===- CompiledImage.cpp - Placement-ready image compilation -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compiles rewrite-backend output into a placement-ready multi-section image.
///
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

#define DEBUG_TYPE "neverd-rewriter"

namespace neverd {

namespace {

struct CapturedFixupReference {
  std::string SectionName;
  CompiledFixupReference Reference;
  bool IsCompactFunctionField = false;
};

void captureFixupReference(std::vector<CapturedFixupReference> &Captured,
                           const llvm::mc_rewrite::FixupCtx &Context,
                           Arch TargetArch) {
  if (Context.SectionName.empty() ||
      (Context.Sym.empty() && Context.SubSym.empty()))
    return;

  // The linker-input compact-unwind section contains three symbolic pointer
  // fields per row.  LLVM can also retain the symbolic end-minus-start
  // expression used to form the adjacent 32-bit range length.  That value is
  // useful while assembling the row, but it is not pointer provenance and the
  // strict final-table parser must never mistake it for such.  Keep exactly
  // the fields whose symbolic identities survive into final metadata.
  if (Context.SectionName == section_names::macho::CompactUnwind) {
    uint64_t PointerWidth = 0;
    switch (TargetArch) {
    case Arch::X64:
    case Arch::AArch64:
      PointerWidth = 8;
      break;
    case Arch::X86:
    case Arch::ARM:
      PointerWidth = 4;
      break;
    default:
      return;
    }
    const uint64_t RecordSize = 3 * PointerWidth + 8;
    const uint64_t FieldOffset = Context.SectionOffset % RecordSize;
    if (FieldOffset != 0 && FieldOffset != PointerWidth + 8 &&
        FieldOffset != 2 * PointerWidth + 8)
      return;
  }

  CapturedFixupReference Item;
  Item.SectionName = Context.SectionName.str();
  Item.Reference.Offset = Context.SectionOffset;
  Item.Reference.Kind = Context.Kind;
  Item.Reference.Symbol = Context.Sym.str();
  Item.Reference.SubtractSymbol = Context.SubSym.str();
  Item.Reference.Addend = Context.Addend;
  Item.Reference.Specifier = Context.Specifier;
  Item.Reference.IsPCRel = Context.IsPCRel;
  Item.Reference.IsResolved = Context.IsResolved;
  Item.Reference.BitWidth = Context.BitWidth;
  if (Context.SectionName == section_names::macho::CompactUnwind) {
    const uint64_t PointerWidth =
        TargetArch == Arch::X86 || TargetArch == Arch::ARM ? 4 : 8;
    const uint64_t RecordSize = 3 * PointerWidth + 8;
    Item.IsCompactFunctionField = Context.SectionOffset % RecordSize == 0;
  }
  Captured.push_back(std::move(Item));
}

bool attachFixupReferences(
    CompiledSection &Section, std::vector<CapturedFixupReference> &Captured,
    llvm::ArrayRef<llvm::mc_rewrite::RewriteFunctionRange> FunctionRanges) {
  for (CapturedFixupReference &Item : Captured) {
    if (Item.SectionName != Section.Name)
      continue;

    if (Item.IsCompactFunctionField) {
      const llvm::mc_rewrite::RewriteFunctionRange *ExactRange = nullptr;
      for (const llvm::mc_rewrite::RewriteFunctionRange &Range :
           FunctionRanges) {
        if (Range.BeginSymbol != Item.Reference.Symbol)
          continue;
        if (ExactRange)
          return false;
        ExactRange = &Range;
      }
      if (ExactRange)
        Item.Reference.FunctionRangeId = ExactRange->Id;
    }

    Section.FixupReferences.push_back(std::move(Item.Reference));
  }
  return true;
}

llvm::Expected<std::map<std::string, uint64_t>>
collectSourceFunctionOriginalVAs(const llvm::Module &Module) {
  std::map<std::string, uint64_t> Result;
  std::set<uint64_t> SeenAddresses;
  for (const llvm::Function &Function : Module) {
    if (Function.isDeclaration())
      continue;
    auto Address = rewrite_source::getOriginalVA(Function);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      continue;
    if (**Address == InvalidVA ||
        !Result.emplace(Function.getName().str(), **Address).second ||
        !SeenAddresses.insert(**Address).second)
      return llvm::createStringError(
          std::errc::invalid_argument,
          "rewrite source identities are duplicate or invalid");
  }
  return Result;
}

bool attachSourceFunctionOriginalVAs(
    CompiledImage &Image, const std::map<std::string, uint64_t> &OriginalVAs) {
  if (!llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
          Image.SourceFunctionOwners))
    return false;
  for (const auto &[SourceFunction, OriginalVA] : OriginalVAs) {
    const size_t Matches =
        llvm::count_if(Image.SourceFunctionOwners, [&](const auto &Owner) {
          return Owner.SourceFunction == SourceFunction;
        });
    if (Matches != 1)
      return false;
    Image.SourceFunctionOriginalVAs.emplace(SourceFunction, OriginalVA);
  }
  return Image.SourceFunctionOriginalVAs.size() == OriginalVAs.size();
}

} // namespace

//===----------------------------------------------------------------------===//
// compileImageForPatch — iterative multi-section image compile
//===----------------------------------------------------------------------===//

static CompiledImage compileImageForPatchImpl(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef, uint32_t)>
        ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple) {
  CompiledImage Out;
  Out.BaseVA = BaseVA;
  Out.TargetArch = TargetArch;
  Out.Format = Fmt;
  const char *DefaultTriple = llvmCodegenTriple(TargetArch, Fmt);
  if (!DefaultTriple)
    return Out;
  const llvm::Triple Triple(
      TargetTriple.empty() ? llvm::StringRef(DefaultTriple) : TargetTriple);
  Out.TargetTriple = Triple.str();
  if (TargetArch == Arch::ARM)
    Out.TargetMode = Triple.getArch() == llvm::Triple::thumb
                         ? InstructionMode::Thumb
                         : InstructionMode::ARM;
  switch (TargetArch) {
  case Arch::X64:
  case Arch::AArch64:
    Out.PointerWidth = 8;
    break;
  case Arch::X86:
  case Arch::ARM:
    Out.PointerWidth = 4;
    break;
  default:
    Out.PointerWidth = 0;
    break;
  }
  Out.ByteOrder = llvm::endianness::little;

  auto OriginalVAs = collectSourceFunctionOriginalVAs(Mod);
  if (!OriginalVAs) {
    llvm::WithColor::error()
        << "compileImageForPatch: " << llvm::toString(OriginalVAs.takeError())
        << "\n";
    return Out;
  }

  auto IsText = [](llvm::StringRef N) {
    return section_names::isTextSectionName(N);
  };

  // Pass 1: compile with every section anchored at BaseVA. For the common
  // single-section (text-only) case this is already final — text sits at
  // BaseVA and there are no cross-section fixups, so every fixup value is
  // correct and we return after one compile (matching the pre-multi-section
  // fast path). When more than one section is emitted the
  // fixup *values* for cross-section references are stale, but section *sizes*
  // (fixups are fixed-width, applied in place) are exact and drive the layout.
  llvm::mc_rewrite::RewriteOptions Pass1;
  Pass1.DeferGlobalFunctionRangeOverlap = true;
  Pass1.Model.TextVA = BaseVA;
  Pass1.Model.ImageBaseVA = ImageBaseVA;
  Pass1.Model.getSectionVA = [&](llvm::StringRef) { return BaseVA; };
  Pass1.Model.resolve = [&](llvm::StringRef S, uint32_t Sp) {
    return ResolveFn(S, Sp);
  };
  std::vector<CapturedFixupReference> Pass1Fixups;
  Pass1.onFixup = [&](const llvm::mc_rewrite::FixupCtx &Context,
                      uint64_t Value) {
    captureFixupReference(Pass1Fixups, Context, TargetArch);
    return Value;
  };
  Codegen CG1;
  auto Pass1Mod = llvm::CloneModule(Mod);
  auto Res1 =
      CG1.compileForRewrite(*Pass1Mod, TargetArch, Pass1, Fmt, TargetTriple);
  if (!Res1.ImageValid)
    return Out;
  if (!Res1.FunctionRangesValid) {
    Out.FunctionRangesValid = false;
    return Out;
  }
  if (Res1.Sections.empty())
    return Out;

  bool HasFixedSection = false;
  for (const auto &S : Res1.Sections)
    if (S.IsAllocated && FixedSectionVAFn(S.Name)) {
      HasFixedSection = true;
      break;
    }

  if (Res1.Sections.size() == 1 && !HasFixedSection) {
    if (!llvm::mc_rewrite::validateRewriteFunctionRanges(
            Res1.FunctionRanges, Res1.FunctionOwnerAddrs)) {
      Out.FunctionRangesValid = false;
      return Out;
    }
    auto &S = Res1.Sections.front();
    if (S.Alignment == 0 || (S.Alignment & (S.Alignment - 1)) != 0 ||
        (S.IsAllocated && BaseVA % S.Alignment != 0)) {
      llvm::WithColor::error()
          << "compileImageForPatch: invalid single-section alignment\n";
      return Out;
    }
    CompiledSection CS{S.Name,
                       0,
                       S.IsAllocated ? BaseVA : 0,
                       static_cast<uint64_t>(S.Bytes.size()),
                       S.Alignment,
                       S.Kind,
                       S.IsAllocated};
    CS.SymbolIndexReferences = std::move(S.SymbolIndexReferences);
    if (!attachFixupReferences(CS, Pass1Fixups, Res1.FunctionRanges)) {
      Out.FunctionRangesValid = false;
      return Out;
    }
    if (!S.IsAllocated) {
      CS.IsInImage = false;
      CS.ExternalBytes = std::move(S.Bytes);
    }
    Out.Sections.push_back(std::move(CS));
    if (S.IsAllocated)
      Out.Bytes = std::move(S.Bytes);
    Out.SymbolAddrs = std::move(Res1.SymbolAddrs);
    Out.FunctionOwnerAddrs = std::move(Res1.FunctionOwnerAddrs);
    Out.FunctionRanges = std::move(Res1.FunctionRanges);
    Out.SourceFunctionOwners = std::move(Res1.SourceFunctionOwners);
    if (!attachSourceFunctionOriginalVAs(Out, *OriginalVAs)) {
      Out.FunctionRangesValid = false;
      return Out;
    }
    Out.Unresolved = std::move(Res1.Unresolved);
    Out.Success = true;
    LLVM_DEBUG(llvm::dbgs()
               << "compileImageForPatch: 1 section, " << Out.Bytes.size()
               << " bytes from VA 0x" << llvm::utohexstr(BaseVA) << "\n");
    return Out;
  }

  // Plan a contiguous layout: text first at BaseVA, then the remaining sections
  // in emission order, each honoring both a 16-byte patch-image floor (covers
  // pointer/blockaddress tables and vector constants) and the alignment
  // reported by MC.  Section
  // spacing uses a per-section MONOTONIC max size (MaxSize, grown each round)
  // rather than this compile's size, so a section never moves backward and the
  // layout cannot oscillate (see the iteration note below).
  const uint64_t Align = 16;
  std::map<std::string, uint64_t> MaxSize;
  auto planLayout =
      [&](const std::vector<llvm::mc_rewrite::RewriteSection> &Secs,
          std::map<std::string, uint64_t> &VA, uint64_t &Total) -> bool {
    VA.clear();
    uint64_t Cur = BaseVA;
    bool Valid = true;
    auto add = [&](const llvm::mc_rewrite::RewriteSection &S) {
      if (!S.IsAllocated)
        return;
      if (std::optional<uint64_t> FixedVA = FixedSectionVAFn(S.Name)) {
        if (S.Alignment == 0 || (S.Alignment & (S.Alignment - 1)) != 0 ||
            *FixedVA % S.Alignment != 0) {
          Valid = false;
          return;
        }
        VA[S.Name] = *FixedVA;
        return;
      }
      uint64_t SectionAlign = std::max<uint64_t>(Align, S.Alignment);
      if (S.Alignment == 0 || (S.Alignment & (S.Alignment - 1)) != 0 ||
          SectionAlign == 0 || (SectionAlign & (SectionAlign - 1)) != 0 ||
          Cur > std::numeric_limits<uint64_t>::max() - (SectionAlign - 1)) {
        Valid = false;
        return;
      }
      uint64_t V = alignUp(Cur, SectionAlign);
      if (MaxSize[S.Name] > std::numeric_limits<uint64_t>::max() - V) {
        Valid = false;
        return;
      }
      VA[S.Name] = V;
      Cur = V + MaxSize[S.Name];
    };
    for (auto &S : Secs)
      if (IsText(S.Name))
        add(S);
    for (auto &S : Secs)
      if (!IsText(S.Name))
        add(S);
    if (!Valid || Cur < BaseVA ||
        Cur - BaseVA > std::numeric_limits<size_t>::max())
      return false;
    Total = Cur - BaseVA;
    return true;
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
  // given.  Each code-generation pass gets a fresh clone of the same input
  // module.  Target lowering is not generally repeatable on already-lowered
  // IR: WinEH preparation, in particular, rewrites catch/cleanup regions and
  // a second pass can reject or discard the transformed function.  Cloning
  // also keeps this layout probe from mutating the caller's module.
  for (auto &S : Res1.Sections)
    MaxSize[S.Name] = std::max<uint64_t>(MaxSize[S.Name], S.Bytes.size());
  std::map<std::string, uint64_t> SectionVA;
  uint64_t TotalSize = 0;
  if (!planLayout(Res1.Sections, SectionVA, TotalSize)) {
    llvm::WithColor::error()
        << "compileImageForPatch: section layout overflows\n";
    return Out;
  }
  if (SectionVA.empty())
    return Out;

  llvm::mc_rewrite::RewriteResult Final;
  std::vector<CapturedFixupReference> FinalFixups;
  bool Converged = false;
  for (int Iter = 0; Iter < 8 && !Converged; ++Iter) {
    llvm::mc_rewrite::RewriteOptions PassN;
    PassN.Model.TextVA = BaseVA;
    PassN.Model.ImageBaseVA = ImageBaseVA;
    PassN.Model.getSectionVA = [&](llvm::StringRef N) -> uint64_t {
      auto It = SectionVA.find(N.str());
      return It != SectionVA.end() ? It->second : BaseVA;
    };
    PassN.Model.resolve = [&](llvm::StringRef S, uint32_t Sp) {
      return ResolveFn(S, Sp);
    };
    std::vector<CapturedFixupReference> IterationFixups;
    PassN.onFixup = [&](const llvm::mc_rewrite::FixupCtx &Context,
                        uint64_t Value) {
      captureFixupReference(IterationFixups, Context, TargetArch);
      return Value;
    };
    Codegen CGn;
    auto IterMod = llvm::CloneModule(Mod);
    auto ResN =
        CGn.compileForRewrite(*IterMod, TargetArch, PassN, Fmt, TargetTriple);
    if (!ResN.ImageValid)
      return Out;
    if (!ResN.FunctionRangesValid) {
      Out.FunctionRangesValid = false;
      return Out;
    }
    if (ResN.Sections.empty())
      return Out;

    // Grow the monotonic sizes from this compile, then re-plan from them.
    for (auto &S : ResN.Sections)
      MaxSize[S.Name] = std::max<uint64_t>(MaxSize[S.Name], S.Bytes.size());
    std::map<std::string, uint64_t> NewVA;
    uint64_t NewTotal = 0;
    if (!planLayout(ResN.Sections, NewVA, NewTotal)) {
      llvm::WithColor::error()
          << "compileImageForPatch: section layout overflows\n";
      return Out;
    }
    if (NewVA == SectionVA) {
      // The layout this compile was given regenerates itself (the max sizes
      // have settled) → its fixups already target the final addresses. Accept
      // it; its actual section bytes (<= the max spacing) are placed at these
      // VAs.
      Final = std::move(ResN);
      FinalFixups = std::move(IterationFixups);
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
    if (!S.IsAllocated) {
      CompiledSection CS{
          S.Name,      0,      0,    static_cast<uint64_t>(S.Bytes.size()),
          S.Alignment, S.Kind, false};
      CS.SymbolIndexReferences = std::move(S.SymbolIndexReferences);
      if (!attachFixupReferences(CS, FinalFixups, Final.FunctionRanges)) {
        Out.FunctionRangesValid = false;
        return Out;
      }
      CS.IsInImage = false;
      CS.ExternalBytes = std::move(S.Bytes);
      Out.Sections.push_back(std::move(CS));
      continue;
    }
    auto VAIt = SectionVA.find(S.Name);
    if (VAIt == SectionVA.end())
      continue;
    const bool IsExternallyPlaced = static_cast<bool>(FixedSectionVAFn(S.Name));
    uint64_t Off = IsExternallyPlaced ? 0 : VAIt->second - BaseVA;
    CompiledSection CS{S.Name,       Off,
                       VAIt->second, static_cast<uint64_t>(S.Bytes.size()),
                       S.Alignment,  S.Kind,
                       true};
    CS.SymbolIndexReferences = std::move(S.SymbolIndexReferences);
    if (!attachFixupReferences(CS, FinalFixups, Final.FunctionRanges)) {
      Out.FunctionRangesValid = false;
      return Out;
    }
    if (IsExternallyPlaced) {
      CS.IsInImage = false;
      CS.ExternalBytes = std::move(S.Bytes);
      Out.Sections.push_back(std::move(CS));
      continue;
    }
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
    Out.Sections.push_back(std::move(CS));
  }

  Out.SymbolAddrs = std::move(Final.SymbolAddrs);
  Out.FunctionOwnerAddrs = std::move(Final.FunctionOwnerAddrs);
  Out.FunctionRanges = std::move(Final.FunctionRanges);
  Out.SourceFunctionOwners = std::move(Final.SourceFunctionOwners);
  if (!attachSourceFunctionOriginalVAs(Out, *OriginalVAs)) {
    Out.FunctionRangesValid = false;
    return Out;
  }
  Out.Unresolved = std::move(Final.Unresolved);
  Out.Success = true;
  LLVM_DEBUG(llvm::dbgs() << "compileImageForPatch: " << SectionVA.size()
                          << " sections, " << TotalSize << " bytes from VA 0x"
                          << llvm::utohexstr(BaseVA) << "\n");
  return Out;
}

CompiledImage compileImageForPatch(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef, uint32_t)>
        ResolveFn,
    uint64_t ImageBaseVA) {
  return compileImageForPatch(Mod, TargetArch, Fmt, BaseVA, ResolveFn,
                              ImageBaseVA, llvm::StringRef());
}

CompiledImage compileImageForPatch(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef, uint32_t)>
        ResolveFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple) {
  auto NoFixedSection = [](llvm::StringRef) -> std::optional<uint64_t> {
    return std::nullopt;
  };
  return compileImageForPatchImpl(Mod, TargetArch, Fmt, BaseVA, ResolveFn,
                                  NoFixedSection, ImageBaseVA, TargetTriple);
}

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef, uint32_t)>
        ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA) {
  return compileImageForPatchWithFixedSectionVAs(
      Mod, TargetArch, Fmt, BaseVA, ResolveFn, FixedSectionVAFn, ImageBaseVA,
      llvm::StringRef());
}

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef, uint32_t)>
        ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple) {
  return compileImageForPatchImpl(Mod, TargetArch, Fmt, BaseVA, ResolveFn,
                                  FixedSectionVAFn, ImageBaseVA, TargetTriple);
}

} // namespace neverd
