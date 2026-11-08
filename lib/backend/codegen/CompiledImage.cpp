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

#include "X86/CompiledSEHStateWrites.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/TargetRegistry.h"
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
#include <tuple>
#include <utility>

#define DEBUG_TYPE "neverd-rewriter"

namespace neverd {

namespace {

#define CODEGEN_PAUTH_TEXT(Name, Text) constexpr char Name[] = Text;
#include "CodeGenPointerAuth.def"

bool supportedSigningOwner(const CompiledImage &Image) {
  for (const auto &Section : Image.Sections)
    for (const auto &Fixup : Section.FixupReferences)
      if (Fixup.PointerAuth &&
          (Image.Format != BinaryFormat::MachO ||
           !llvm::Triple(Image.TargetTriple).isArm64e() ||
           Image.InputKind != RewriteInputKind::ReplacementIR)) {
        llvm::WithColor::error() << kPAuthInstallationOwner;
        return false;
      }
  return true;
}

struct CapturedFixupReference {
  std::string SectionName;
  CompiledFixupReference Reference;
  bool IsCompactFunctionField = false;
};

struct SourceDisplacements {
  bool IsMachO = false;
  bool IsI386ELF = false;
  std::map<std::pair<uint64_t, std::string>, uint64_t> Targets;
  std::map<std::pair<uint64_t, std::string>, uint64_t> MachOGOTData;
  std::map<std::pair<uint64_t, std::string>, uint64_t> I386GOTAnchors;

  void record(const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request,
              uint64_t Address) {
    if (Request.IsSubtrahend)
      return;
    if (Request.SpecifierName == "PLT")
      Targets[{Request.FixupVA, Request.Symbol.str()}] = Address;
    if (IsMachO && Request.SpecifierName == "GOTPCREL" &&
        Request.FixupKindName == "FK_Data_4" && !Request.IsPCRel)
      MachOGOTData[{Request.FixupVA, Request.Symbol.str()}] = Address;
    if (IsI386ELF && Request.Symbol == "_GLOBAL_OFFSET_TABLE_" &&
        !Request.Specifier)
      I386GOTAnchors[{Request.FixupVA, Request.Symbol.str()}] = Address;
  }

  bool validate(const CompiledImage &Image) const {
    for (const auto &Section : Image.Sections) {
      if (!Section.IsAllocated || !Section.IsInImage)
        continue;
      for (const auto &Fixup : Section.FixupReferences) {
        auto Target = Targets.find({Section.VA + Fixup.Offset, Fixup.Symbol});
        if (Target == Targets.end())
          continue;
        if (!Fixup.IsPCRel || Fixup.BitWidth != 32 ||
            !Fixup.SubtractSymbol.empty() ||
            !rangeInBounds(Fixup.Offset, 4, Section.Size))
          return false;
        const auto Displacement =
            readLE<int32_t>(Image.Bytes.data() + Section.Offset + Fixup.Offset);
        if (Section.VA + Fixup.Offset + 4 + Displacement !=
            Target->second + static_cast<uint64_t>(Fixup.Addend)) {
          llvm::WithColor::error()
              << "compileImageForPatch: LLVM encoded an invalid x64 PLT "
                 "displacement; rebuild the corrected LLVM fork (the r3 "
                 "prebuilt has an AArch64 specifier collision)\n";
          return false;
        }
      }
    }
    for (const auto &Section : Image.Sections) {
      if (!Section.IsAllocated)
        continue;
      llvm::ArrayRef<uint8_t> Bytes;
      if (Section.IsInImage) {
        if (!rangeInBounds(Section.Offset, Section.Size, Image.Bytes.size()))
          return false;
        Bytes = llvm::ArrayRef<uint8_t>(Image.Bytes)
                    .slice(Section.Offset, Section.Size);
      } else {
        if (Section.ExternalBytes.size() != Section.Size)
          return false;
        Bytes = Section.ExternalBytes;
      }
      for (const auto &Fixup : Section.FixupReferences) {
        auto Anchor =
            I386GOTAnchors.find({Section.VA + Fixup.Offset, Fixup.Symbol});
        if (Anchor != I386GOTAnchors.end()) {
          if (Fixup.BitWidth != 32 || !Fixup.IsResolved ||
              !Fixup.SubtractSymbol.empty() ||
              !rangeInBounds(Fixup.Offset, 4, Section.Size))
            return false;
          const uint32_t Encoded =
              readLE<uint32_t>(Bytes.data() + Fixup.Offset);
          const uint32_t Expected = static_cast<uint32_t>(
              Anchor->second + static_cast<uint64_t>(Fixup.Addend) -
              (Section.VA + Fixup.Offset));
          if (Encoded != Expected) {
            llvm::WithColor::error()
                << "compileImageForPatch: LLVM encoded an invalid i386 GOT "
                   "anchor displacement; rebuild the corrected LLVM fork\n";
            return false;
          }
        }
        auto Target =
            MachOGOTData.find({Section.VA + Fixup.Offset, Fixup.Symbol});
        if (Target == MachOGOTData.end())
          continue;
        if (Fixup.IsPCRel || Fixup.Kind != llvm::FK_Data_4 ||
            Fixup.BitWidth != 32 || !Fixup.SubtractSymbol.empty() ||
            !rangeInBounds(Fixup.Offset, 4, Section.Size))
          return false;
        const auto Displacement = readLE<int32_t>(Bytes.data() + Fixup.Offset);
        if (Section.VA + Fixup.Offset + 4 + Displacement !=
            Target->second + static_cast<uint64_t>(Fixup.Addend)) {
          llvm::WithColor::error()
              << "compileImageForPatch: LLVM encoded an invalid Mach-O x64 "
                 "GOT data displacement; rebuild the corrected LLVM fork\n";
          return false;
        }
      }
    }
    return true;
  }
};

// AArch64 ELF colon specifiers are absent from MCAsmInfo's @-name table.
// LLVM 22 AArch64::Specifier combines S_GOT, S_PAGE/PAGEOFF and S_NC here.
// Normalize once for both live resolution and captured relocation receipts.
static llvm::StringRef sourceSpecifierName(Arch Architecture,
                                           BinaryFormat Format,
                                           unsigned Specifier,
                                           llvm::StringRef Known) {
  if (!Known.empty() || Architecture != Arch::AArch64 ||
      Format != BinaryFormat::ELF)
    return Known;
  switch (Specifier) {
  case 0x001: // S_ABS
    return "ABS";
  case 0x001 | 0x010: // S_ABS | S_PAGE
    return "ABS_PAGE";
  case 0x001 | 0x020 | 0x100: // S_ABS | S_PAGEOFF | S_NC
    return "ABS_LO12";
  case 0x004 | 0x010:
    return "GOT_PAGE";
  case 0x004 | 0x020 | 0x100:
    return "GOT_LO12";
  default:
    return Known;
  }
}

struct PatchSymbolResolvers {
  PatchSymbolResolver Legacy;
  ContextualPatchSymbolResolver Contextual;

  void install(llvm::mc_rewrite::RewriteAddressModel &Model, Arch Architecture,
               BinaryFormat Format, SourceDisplacements *PLT = nullptr) const {
    if (Contextual) {
      ContextualPatchSymbolResolver Resolver = Contextual;
      Model.resolveWithContext =
          [Resolver, Architecture, Format,
           PLT](const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request) {
            auto Named = Request;
            Named.SpecifierName = sourceSpecifierName(
                Architecture, Format, Request.Specifier, Request.SpecifierName);
            auto Address = Resolver(Named);
            if (PLT && Address)
              PLT->record(Named, *Address);
            return Address;
          };
      return;
    }

    if (Legacy) {
      PatchSymbolResolver Resolver = Legacy;
      if (!PLT) {
        Model.resolve = std::move(Resolver);
        return;
      }
      Model.resolveWithContext =
          [Resolver,
           PLT](const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request) {
            auto Address = Resolver(Request.Symbol, Request.Specifier);
            if (PLT && Address)
              PLT->record(Request, *Address);
            return Address;
          };
    }
  }
};

struct WinEHSemanticEmissionShape {
  llvm::mc_rewrite::RewriteWinEHSemanticToken Token;
  llvm::mc_rewrite::RewriteWinEHSemanticEncoding Encoding =
      llvm::mc_rewrite::RewriteWinEHSemanticEncoding::SEH;
  std::string SourceFunction;
  std::string OwnerSymbol;
  std::string ContainerSymbol;
  std::string BeginSymbol;
  std::string EndSymbol;
  std::string HandlerSymbol;
  uint32_t RecordSize = 0;
  bool HasProtectedRange = false;
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  std::string ContainerEndSymbol;
  std::string FilterSymbol;
  uint32_t GeneratedState = UINT32_MAX;
  int32_t EnclosingState = -1;
  std::array<int32_t, 4> RegistrationCookieOffsets{};
#endif
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  bool HasX86CxxLayout = false;
  std::array<std::pair<std::string, std::string>, 4> CxxTableSymbols;
  std::array<uint64_t, 4> CxxTableSizes{};
  std::array<int64_t, 4> CxxFrame{};
#endif
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
  std::string CxxRegistrationHandlerSymbol;
  std::array<int64_t, 3> CxxRegistrationFrame{};
#endif

  friend bool operator==(const WinEHSemanticEmissionShape &,
                         const WinEHSemanticEmissionShape &) = default;
};

std::vector<WinEHSemanticEmissionShape> collectWinEHSemanticEmissionShape(
    llvm::ArrayRef<llvm::mc_rewrite::RewriteWinEHSemanticRecord> Records) {
  std::vector<WinEHSemanticEmissionShape> Shape;
  Shape.reserve(Records.size());
  for (const auto &Record : Records) {
    const bool HasProtectedRange =
        !Record.BeginSymbol.empty() || !Record.EndSymbol.empty();
    Shape.push_back({Record.Token, Record.Encoding, Record.SourceFunction,
                     Record.OwnerSymbol, Record.ContainerSymbol,
                     Record.BeginSymbol, Record.EndSymbol, Record.HandlerSymbol,
                     Record.RecordSize, HasProtectedRange});
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
    Shape.back().ContainerEndSymbol = Record.ContainerEndSymbol;
    Shape.back().FilterSymbol = Record.FilterSymbol;
    Shape.back().GeneratedState = Record.GeneratedState;
    Shape.back().EnclosingState = Record.EnclosingState;
    Shape.back().RegistrationCookieOffsets = Record.RegistrationCookieOffsets;
#endif
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
    if (Record.X86CxxLayout) {
      auto &Item = Shape.back();
      Item.HasX86CxxLayout = true;
      Item.CxxFrame = Record.X86CxxLayout->Frame;
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
      Item.CxxRegistrationHandlerSymbol =
          Record.X86CxxLayout->RegistrationHandlerSymbol;
      Item.CxxRegistrationFrame = Record.X86CxxLayout->RegistrationFrame;
#endif
      for (unsigned I = 0; I != 4; ++I) {
        const auto &Table = Record.X86CxxLayout->Tables[I];
        Item.CxxTableSymbols[I] = {Table.BeginSymbol, Table.EndSymbol};
        Item.CxxTableSizes[I] = Table.EndVA - Table.BeginVA;
      }
    }
#endif
  }
  llvm::sort(Shape, [](const WinEHSemanticEmissionShape &Left,
                       const WinEHSemanticEmissionShape &Right) {
    return std::tie(Left.SourceFunction, Left.OwnerSymbol, Left.ContainerSymbol,
                    Left.BeginSymbol, Left.EndSymbol, Left.HandlerSymbol,
                    Left.Token.Kind, Left.Token.Region, Left.Token.Clause,
                    Left.Token.Digest, Left.Encoding, Left.RecordSize,
                    Left.HasProtectedRange
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
                    ,
                    Left.ContainerEndSymbol, Left.FilterSymbol,
                    Left.GeneratedState, Left.EnclosingState,
                    Left.RegistrationCookieOffsets
#endif
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
                    ,
                    Left.HasX86CxxLayout, Left.CxxTableSymbols,
                    Left.CxxTableSizes, Left.CxxFrame
#endif
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
                    ,
                    Left.CxxRegistrationHandlerSymbol, Left.CxxRegistrationFrame
#endif
                    ) <
           std::tie(Right.SourceFunction, Right.OwnerSymbol,
                    Right.ContainerSymbol, Right.BeginSymbol, Right.EndSymbol,
                    Right.HandlerSymbol, Right.Token.Kind, Right.Token.Region,
                    Right.Token.Clause, Right.Token.Digest, Right.Encoding,
                    Right.RecordSize, Right.HasProtectedRange
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
                    ,
                    Right.ContainerEndSymbol, Right.FilterSymbol,
                    Right.GeneratedState, Right.EnclosingState,
                    Right.RegistrationCookieOffsets
#endif
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
                    ,
                    Right.HasX86CxxLayout, Right.CxxTableSymbols,
                    Right.CxxTableSizes, Right.CxxFrame
#endif
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
                    ,
                    Right.CxxRegistrationHandlerSymbol,
                    Right.CxxRegistrationFrame
#endif
           );
  });
  return Shape;
}

void captureFixupReference(std::vector<CapturedFixupReference> &Captured,
                           const llvm::mc_rewrite::FixupCtx &Context,
                           Arch TargetArch, uint64_t Value) {
  bool Authenticated = false;
#if __has_include("llvm/MC/MCPointerAuthInfo.h")
  Authenticated = Context.PointerAuth.has_value();
#endif
  if (Context.SectionName.empty() ||
      (!Authenticated && Context.Sym.empty() && Context.SubSym.empty()))
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
  Item.Reference.ResolvedValue = Value;
#if __has_include("llvm/MC/MCPointerAuthInfo.h")
  if (Context.PointerAuth) {
    const auto &Auth = *Context.PointerAuth;
    Item.Reference.PointerAuth = CompiledPointerAuthentication{
        uint8_t(Auth.Key), Auth.Discriminator, Auth.HasAddressDiversity};
  }
#endif
#ifdef LLVM_NEVERD_X86_CXX_HANDLER_RECEIPTS
  Item.Reference.KindName = Context.KindName.str();
#endif
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
    CompiledImage &Image, const std::map<std::string, uint64_t> &OriginalVAs,
    const llvm::Module &Module) {
  if (!llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
          Image.SourceFunctionOwners))
    return false;
  if (Image.InputKind == RewriteInputKind::ReplacementIR &&
      Image.TargetArch == Arch::ARM) {
    for (const auto &Owner : Image.SourceFunctionOwners) {
      if (Owner.Kind !=
          llvm::mc_rewrite::RewriteSourceFunctionOwnerKind::FunctionEntry)
        continue;
      const auto *Function = Module.getFunction(Owner.SourceFunction);
      if (!Function)
        return false;
      InstructionMode Mode = Image.TargetMode;
      const auto Attribute = Function->getFnAttribute("target-features");
      if (Attribute.isStringAttribute()) {
        const auto Features = Attribute.getValueAsString();
        const bool Thumb = Features.contains("+thumb-mode");
        const bool ARM = Features.contains("-thumb-mode");
        if (Thumb && ARM)
          return false;
        if (Thumb || ARM)
          Mode = Thumb ? InstructionMode::Thumb : InstructionMode::ARM;
      }
      if (!Image.ReplacementFunctionModes.emplace(Owner.OwnerVA, Mode).second)
        return false;
    }
  }
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

std::optional<uint64_t> nearestAlignedVA(uint64_t VA, uint64_t Alignment) {
  if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
    return std::nullopt;
  const uint64_t Floor = VA & ~(Alignment - 1);
  if (Floor == VA)
    return VA;
  if (Floor > std::numeric_limits<uint64_t>::max() - Alignment)
    return Floor;
  const uint64_t Ceil = Floor + Alignment;
  // Prefer the upper address on an exact tie: a forward PC-relative reference
  // includes the instruction-width bias and remains inside a signed range.
  return VA - Floor < Ceil - VA ? Floor : Ceil;
}

} // namespace

//===----------------------------------------------------------------------===//
// compileImageForPatch — iterative multi-section image compile
//===----------------------------------------------------------------------===//

static CompiledImage compileImageForPatchImpl(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    PatchSymbolResolvers Resolvers,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple,
    RewriteInputKind InputKind) {
  SourceDisplacements PLT;
  PLT.IsMachO = Fmt == BinaryFormat::MachO;
  PLT.IsI386ELF = TargetArch == Arch::X86 && Fmt == BinaryFormat::ELF;
  SourceDisplacements *PLTCheck =
      ((TargetArch == Arch::X64 &&
        (Fmt == BinaryFormat::ELF || Fmt == BinaryFormat::MachO)) ||
       PLT.IsI386ELF) &&
              InputKind == RewriteInputKind::ReplacementIR
          ? &PLT
          : nullptr;
  CompiledImage Out;
  Out.InputKind = InputKind;
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

  // The sizing compile does not know native section alignments until MC has
  // emitted the sections.  Keep text at its required final VA, but give every
  // provisional non-text section a nearby, conservatively aligned anchor.  A
  // function entry commonly has only instruction alignment (4 bytes on
  // AArch64), while a generated literal pool can require 16 bytes; anchoring
  // both at the entry makes the target reject the fixup before its section
  // alignment can be discovered.  Explicit over-aligned globals raise the
  // probe alignment beyond the 64-KiB platform floor.
  uint64_t ProbeSectionAlignment = 64 * 1024;
  for (const llvm::GlobalVariable &GV : Mod.globals())
    if (llvm::MaybeAlign Alignment = GV.getAlign())
      ProbeSectionAlignment =
          std::max<uint64_t>(ProbeSectionAlignment, Alignment->value());
  const std::optional<uint64_t> ProbeSectionVA =
      nearestAlignedVA(BaseVA, ProbeSectionAlignment);
  if (!ProbeSectionVA)
    return Out;

  // Pass 1: compile text at BaseVA and provisional non-text sections at their
  // aligned probe VA. For the common single-section (text-only) case this is
  // already final — text sits at BaseVA and there are no cross-section fixups,
  // so every fixup value is correct and we return after one compile (matching
  // the pre-multi-section fast path). When more than one section is emitted the
  // fixup *values* for cross-section references are stale, but section *sizes*
  // (fixups are fixed-width, applied in place) are exact and drive the layout.
  llvm::mc_rewrite::RewriteOptions Pass1;
  Pass1.DeferGlobalFunctionRangeOverlap = true;
  Pass1.Model.TextVA = BaseVA;
  Pass1.Model.ImageBaseVA = ImageBaseVA;
  Pass1.Model.getSectionVA = [&](llvm::StringRef Name) {
    if (std::optional<uint64_t> FixedVA = FixedSectionVAFn(Name))
      return *FixedVA;
    return IsText(Name) ? BaseVA : *ProbeSectionVA;
  };
  Resolvers.install(Pass1.Model, TargetArch, Fmt, PLTCheck);
  std::vector<CapturedFixupReference> Pass1Fixups;
  Pass1.onFixup = [&](const llvm::mc_rewrite::FixupCtx &Context,
                      uint64_t Value) {
    captureFixupReference(Pass1Fixups, Context, TargetArch, Value);
    // Authentication is installed only after final native placement.
#if __has_include("llvm/MC/MCPointerAuthInfo.h")
    if (Context.PointerAuth)
      return uint64_t(0);
#endif
    return Value;
  };
  Codegen CG1;
  auto Pass1Mod = llvm::CloneModule(Mod);
  auto Res1 = CG1.compileForRewrite(*Pass1Mod, TargetArch, Pass1, Fmt,
                                    TargetTriple, InputKind);
  if (!Res1.ImageValid) {
    llvm::WithColor::error()
        << "compileImageForPatch: sizing compile produced an invalid image\n";
    return Out;
  }
  if (InputKind == RewriteInputKind::ReplacementIR &&
      !Res1.Unresolved.empty()) {
    llvm::WithColor::error()
        << "compileImageForPatch: unresolved replacement symbol: "
        << Res1.Unresolved.front() << "\n";
    return Out;
  }
  if (!Res1.FunctionRangesValid) {
    llvm::WithColor::error()
        << "compileImageForPatch: sizing compile produced invalid function "
           "provenance\n";
    Out.FunctionRangesValid = false;
    return Out;
  }
  if (!Res1.WinEHSemanticsValid) {
    llvm::WithColor::error()
        << "compileImageForPatch: sizing compile produced invalid Windows EH "
           "semantic provenance\n";
    Out.WinEHSemanticsValid = false;
    return Out;
  }
  if (Res1.Sections.empty()) {
    llvm::WithColor::error()
        << "compileImageForPatch: sizing compile emitted no sections\n";
    return Out;
  }
  std::string TargetError;
  const auto *Backend = llvm::TargetRegistry::lookupTarget(Triple, TargetError);
  if (!Backend)
    return Out;
  std::unique_ptr<llvm::MCRegisterInfo> Registers(
      Backend->createMCRegInfo(Triple));
  std::unique_ptr<llvm::MCAsmInfo> AsmInfo(
      Registers ? Backend->createMCAsmInfo(*Registers, Triple,
                                           llvm::MCTargetOptions())
                : nullptr);
  if (!AsmInfo)
    return Out;
  auto NameSpecifiers = [&](std::vector<CapturedFixupReference> &Fixups) {
    for (auto &Fixup : Fixups) {
      Fixup.Reference.SpecifierName =
          AsmInfo->getSpecifierNameOrEmpty(Fixup.Reference.Specifier).str();
      Fixup.Reference.SpecifierName =
          sourceSpecifierName(TargetArch, Fmt, Fixup.Reference.Specifier,
                              Fixup.Reference.SpecifierName)
              .str();
    }
  };
  NameSpecifiers(Pass1Fixups);
  const std::vector<WinEHSemanticEmissionShape> Pass1WinEHSemanticShape =
      collectWinEHSemanticEmissionShape(Res1.WinEHSemanticRecords);
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  const auto Pass1SEHStateShape = collectX86SEHStateEmissionShape(Res1);
  if (!Pass1SEHStateShape) {
    llvm::WithColor::error()
        << "compileImageForPatch: sizing compile produced invalid x86 SEH "
           "state-write provenance\n";
    Out.WinX86SEHStateWritesValid = false;
    return Out;
  }
#endif

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
    if (!llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
            Res1.WinEHSemanticRecords, Res1.SourceFunctionOwners,
            Res1.FunctionRanges, Res1.FunctionOwnerAddrs)) {
      Out.WinEHSemanticsValid = false;
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
    Out.WinEHSemanticRecords = std::move(Res1.WinEHSemanticRecords);
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
    Out.WinX86SEHFrames = std::move(Res1.WinX86SEHFrames);
    Out.WinX86SEHStateWrites = std::move(Res1.WinX86SEHStateWrites);
    if (!validateCompiledX86SEHStateWrites(Out)) {
      llvm::WithColor::error()
          << "compileImageForPatch: final x86 SEH state writes lost code "
             "or source ownership\n";
      Out.WinX86SEHStateWritesValid = false;
      return Out;
    }
#endif
    if (!attachSourceFunctionOriginalVAs(Out, *OriginalVAs, Mod)) {
      llvm::WithColor::error()
          << "compileImageForPatch: sizing compile lost exact source "
             "function ownership\n";
      Out.FunctionRangesValid = false;
      return Out;
    }
    Out.Unresolved = std::move(Res1.Unresolved);
    Out.Success =
        supportedSigningOwner(Out) && (!PLTCheck || PLTCheck->validate(Out));
    LLVM_DEBUG(llvm::dbgs()
               << "compileImageForPatch: 1 section, " << Out.Bytes.size()
               << " bytes from VA 0x" << llvm::utohexstr(BaseVA) << "\n");
    return Out;
  }

  // Plan a contiguous layout: text first at BaseVA using its native instruction
  // alignment, then the remaining sections in emission order, each honoring
  // both a 16-byte patch-image floor (covers pointer/blockaddress tables and
  // vector constants) and the alignment reported by MC.  Section
  // spacing uses a per-section MONOTONIC max size (MaxSize, grown each round)
  // rather than this compile's size, so a section never moves backward and the
  // layout cannot oscillate (see the iteration note below).
  const uint64_t Align = 16;
  const bool SplitWritable = InputKind == RewriteInputKind::ReplacementIR;
  auto IsWritable = [](const llvm::mc_rewrite::RewriteSection &Section) {
    return Section.Kind == llvm::mc_rewrite::RewriteSectionKind::WritableData ||
           Section.Kind ==
               llvm::mc_rewrite::RewriteSectionKind::UninitializedData;
  };
  std::map<std::string, uint64_t> MaxSize;
  auto planLayout =
      [&](const std::vector<llvm::mc_rewrite::RewriteSection> &Secs,
          std::map<std::string, uint64_t> &VA, uint64_t &Total) -> bool {
    VA.clear();
    uint64_t Cur = BaseVA;
    bool Valid = true;
    bool PlacedWritable = false;
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
      uint64_t SectionAlign =
          IsText(S.Name) ? S.Alignment : std::max<uint64_t>(Align, S.Alignment);
      if (SplitWritable && IsWritable(S) && !PlacedWritable) {
        const uint64_t Page = sourcePatchStorageAlignment(TargetArch, Fmt);
        // Keep space at the end of RX storage for a format-owned unwind
        // search table, then start persistent state on a separate page.
        if (Cur > std::numeric_limits<uint64_t>::max() - Page) {
          Valid = false;
          return;
        }
        Cur += Page;
        SectionAlign = std::max(SectionAlign, Page);
        PlacedWritable = true;
      }
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
      if (!IsText(S.Name) && (!SplitWritable || !IsWritable(S)))
        add(S);
    if (SplitWritable)
      for (auto &S : Secs)
        if (!IsText(S.Name) && IsWritable(S))
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
  if (SectionVA.empty()) {
    llvm::WithColor::error()
        << "compileImageForPatch: section layout has no allocated sections\n";
    return Out;
  }

  llvm::mc_rewrite::RewriteResult Final;
  std::vector<CapturedFixupReference> FinalFixups;
  bool Converged = false;
  for (int Iter = 0;
       Iter < ::neverd::limits::backend::
                  kCompiledImageCompileImageForPatchImplIterLimit &&
       !Converged;
       ++Iter) {
    llvm::mc_rewrite::RewriteOptions PassN;
    PassN.Model.TextVA = BaseVA;
    PassN.Model.ImageBaseVA = ImageBaseVA;
    PassN.Model.getSectionVA = [&](llvm::StringRef N) -> uint64_t {
      auto It = SectionVA.find(N.str());
      return It != SectionVA.end() ? It->second : BaseVA;
    };
    Resolvers.install(PassN.Model, TargetArch, Fmt, PLTCheck);
    std::vector<CapturedFixupReference> IterationFixups;
    PassN.onFixup = [&](const llvm::mc_rewrite::FixupCtx &Context,
                        uint64_t Value) {
      captureFixupReference(IterationFixups, Context, TargetArch, Value);
      // Authentication is installed only after final native placement.
#if __has_include("llvm/MC/MCPointerAuthInfo.h")
      if (Context.PointerAuth)
        return uint64_t(0);
#endif
      return Value;
    };
    Codegen CGn;
    auto IterMod = llvm::CloneModule(Mod);
    auto ResN = CGn.compileForRewrite(*IterMod, TargetArch, PassN, Fmt,
                                      TargetTriple, InputKind);
    NameSpecifiers(IterationFixups);
    if (!ResN.ImageValid) {
      llvm::WithColor::error()
          << "compileImageForPatch: layout compile produced an invalid image\n";
      return Out;
    }
    if (!ResN.FunctionRangesValid) {
      llvm::WithColor::error()
          << "compileImageForPatch: layout compile produced invalid function "
             "provenance\n";
      Out.FunctionRangesValid = false;
      return Out;
    }
    if (!ResN.WinEHSemanticsValid) {
      llvm::WithColor::error()
          << "compileImageForPatch: layout compile produced invalid Windows "
             "EH semantic provenance\n";
      Out.WinEHSemanticsValid = false;
      return Out;
    }
    if (collectWinEHSemanticEmissionShape(ResN.WinEHSemanticRecords) !=
        Pass1WinEHSemanticShape) {
      llvm::WithColor::error()
          << "compileImageForPatch: Windows EH semantic emission shape "
             "changed during layout\n";
      Out.WinEHSemanticsValid = false;
      return Out;
    }
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
    const auto SEHStateShape = collectX86SEHStateEmissionShape(ResN);
    if (!SEHStateShape || *SEHStateShape != *Pass1SEHStateShape) {
      llvm::WithColor::error()
          << "compileImageForPatch: x86 SEH state-write provenance changed "
             "during layout\n";
      Out.WinX86SEHStateWritesValid = false;
      return Out;
    }
#endif
    if (ResN.Sections.empty()) {
      llvm::WithColor::error()
          << "compileImageForPatch: layout compile emitted no sections\n";
      return Out;
    }

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
  Out.WinEHSemanticRecords = std::move(Final.WinEHSemanticRecords);
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  Out.WinX86SEHFrames = std::move(Final.WinX86SEHFrames);
  Out.WinX86SEHStateWrites = std::move(Final.WinX86SEHStateWrites);
  if (!validateCompiledX86SEHStateWrites(Out)) {
    llvm::WithColor::error()
        << "compileImageForPatch: final x86 SEH state writes lost code "
           "or source ownership\n";
    Out.WinX86SEHStateWritesValid = false;
    return Out;
  }
#endif
  if (!attachSourceFunctionOriginalVAs(Out, *OriginalVAs, Mod)) {
    llvm::WithColor::error()
        << "compileImageForPatch: layout compile lost exact source "
           "function ownership\n";
    Out.FunctionRangesValid = false;
    return Out;
  }
  Out.Unresolved = std::move(Final.Unresolved);
  if (InputKind == RewriteInputKind::ReplacementIR && !Out.Unresolved.empty()) {
    llvm::WithColor::error()
        << "compileImageForPatch: unresolved replacement symbol: "
        << Out.Unresolved.front() << "\n";
    return Out;
  }
  Out.Success =
      supportedSigningOwner(Out) && (!PLTCheck || PLTCheck->validate(Out));
  LLVM_DEBUG(llvm::dbgs() << "compileImageForPatch: " << SectionVA.size()
                          << " sections, " << TotalSize << " bytes from VA 0x"
                          << llvm::utohexstr(BaseVA) << "\n");
  return Out;
}

CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   PatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA) {
  return compileImageForPatch(Mod, TargetArch, Fmt, BaseVA, ResolveFn,
                              ImageBaseVA, llvm::StringRef());
}

CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   PatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA,
                                   llvm::StringRef TargetTriple,
                                   RewriteInputKind InputKind) {
  auto NoFixedSection = [](llvm::StringRef) -> std::optional<uint64_t> {
    return std::nullopt;
  };
  return compileImageForPatchImpl(
      Mod, TargetArch, Fmt, BaseVA, PatchSymbolResolvers{ResolveFn, nullptr},
      NoFixedSection, ImageBaseVA, TargetTriple, InputKind);
}

CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   ContextualPatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA) {
  return compileImageForPatch(Mod, TargetArch, Fmt, BaseVA, ResolveFn,
                              ImageBaseVA, llvm::StringRef());
}

CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   ContextualPatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA,
                                   llvm::StringRef TargetTriple,
                                   RewriteInputKind InputKind) {
  auto NoFixedSection = [](llvm::StringRef) -> std::optional<uint64_t> {
    return std::nullopt;
  };
  return compileImageForPatchImpl(
      Mod, TargetArch, Fmt, BaseVA, PatchSymbolResolvers{nullptr, ResolveFn},
      NoFixedSection, ImageBaseVA, TargetTriple, InputKind);
}

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    PatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA) {
  return compileImageForPatchWithFixedSectionVAs(
      Mod, TargetArch, Fmt, BaseVA, ResolveFn, FixedSectionVAFn, ImageBaseVA,
      llvm::StringRef());
}

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    PatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple,
    RewriteInputKind InputKind) {
  return compileImageForPatchImpl(
      Mod, TargetArch, Fmt, BaseVA, PatchSymbolResolvers{ResolveFn, nullptr},
      FixedSectionVAFn, ImageBaseVA, TargetTriple, InputKind);
}

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    ContextualPatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA) {
  return compileImageForPatchWithFixedSectionVAs(
      Mod, TargetArch, Fmt, BaseVA, ResolveFn, FixedSectionVAFn, ImageBaseVA,
      llvm::StringRef());
}

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    ContextualPatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple,
    RewriteInputKind InputKind) {
  return compileImageForPatchImpl(
      Mod, TargetArch, Fmt, BaseVA, PatchSymbolResolvers{nullptr, ResolveFn},
      FixedSectionVAFn, ImageBaseVA, TargetTriple, InputKind);
}

} // namespace neverd
