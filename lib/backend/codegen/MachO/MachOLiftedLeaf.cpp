//===- MachOLiftedLeaf.cpp - Verified Darwin register-only leaf images
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd {
namespace {
#define MACHO_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#define MACHO_LEAF_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "MachOLiftedLeaf.def"

llvm::Error refuse(const char *Reason) {
  return llvm::createStringError(llvm::errc::not_supported, Reason);
}

bool registerOnly(const DecodedInsn &I) {
  switch (I.Id) {
#define MACHO_LEAF_INSTRUCTION(Name) case Name:
#include "MachOLiftedLeaf.def"
    break;
  default:
    return false;
  }
  const auto &Detail = I.Raw->detail->aarch64;
  for (const auto &Op : llvm::ArrayRef(Detail.operands, Detail.op_count)) {
    if (Op.type == AARCH64_OP_IMM)
      continue;
    if (Op.type != AARCH64_OP_REG)
      return false;
    const auto R = Op.reg;
    if (Op.access == CS_AC_READ) {
      switch (R) {
#define NEVERD_DARWIN_ENTRY_REGISTER(X, W, Name, Reservation)                  \
  case AARCH64_REG_##X:                                                        \
  case AARCH64_REG_##W:                                                        \
    continue;
#include "neverd/backend/llvm/AArch64/LLVMDarwinEntryRegister.def"
      default:
        break;
      }
    }
    if (!((R >= AARCH64_REG_X0 && R <= AARCH64_REG_X17) ||
          (R >= AARCH64_REG_W0 && R <= AARCH64_REG_W17) ||
          (R >= AARCH64_REG_S0 && R <= AARCH64_REG_S7) ||
          (R >= AARCH64_REG_D0 && R <= AARCH64_REG_D7) ||
          R == AARCH64_REG_XZR || R == AARCH64_REG_WZR))
      return false;
  }
  return true;
}

llvm::Error verifyLeaves(const BinaryImage &Original, const Section &Text,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         const CompiledImage &Generated,
                         llvm::ArrayRef<NativeMachOCompactRange> Ranges) {
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  const CompiledSection *Code = nullptr;
  for (const auto &Section : Generated.Sections) {
    if (Section.Kind != Kind::Code) {
      if (Section.Name != section_names::macho::CompactUnwind &&
          Section.Name != section_names::macho::EhFrame)
        return refuse(kLeafCode);
      continue;
    }
    if (Code || Section.Name != section_names::macho::Text ||
        !Section.IsInImage || !Section.FixupReferences.empty())
      return refuse(kLeafCode);
    Code = &Section;
  }
  if (!Code || Ranges.size() != Mappings.size() || Code->Size != Text.Size)
    return llvm::createStringError(
        llvm::errc::not_supported,
        llvm::formatv(kGeneratedCoverage, Ranges.size(), Mappings.size(),
                      Code ? Code->Size : 0, Text.Size)
            .str());
  std::map<va_t, const NativeMachOCompactRange *> ByAddress;
  for (const auto &R : Ranges)
    if (!ByAddress.emplace(R.BeginVA, &R).second)
      return refuse(kLeafCoverage);

  Decoder Decode;
  if (!Decode.init(Original))
    return refuse(kLeafCode);
  for (size_t I = 0; I < Mappings.size(); ++I) {
    const auto &Mapping = Mappings[I];
    const va_t Start = Mapping.OriginalVA;
    const va_t End = I + 1 == Mappings.size() ? Text.VA + Text.Size
                                              : Mappings[I + 1].OriginalVA;
    const auto *Frame = Original.ExceptionMetadata.findFunction(Start);
    const auto Owner = Generated.SymbolAddrs.find(Mapping.Symbol);
    if (Owner == Generated.SymbolAddrs.end())
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kGeneratedSymbol, Mapping.Symbol).str());
    const auto Range = ByAddress.extract(Owner->second);
    if (Range.empty() || Range.mapped()->EndVA - Owner->second != End - Start ||
        Owner->second < Code->VA ||
        !rangeInBounds(Owner->second - Code->VA, End - Start, Code->Size))
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kGeneratedRange, Mapping.Symbol, Start, End).str());
    if (!Frame || !Frame->describesUnwindOnly() || !Frame->Compact ||
        Frame->CodeRange.End < End ||
        Frame->Compact->NativeEncoding != macho_unwind::kARM64ModeFrameless ||
        Range.mapped()->Encoding != macho_unwind::kARM64ModeFrameless)
      return refuse(kLeafFrame);

    const auto Source = llvm::ArrayRef(Original.Raw)
                            .slice(Text.FileOff + Start - Text.VA, End - Start);
    const auto Emitted =
        llvm::ArrayRef(Generated.Bytes)
            .slice(Code->Offset + Owner->second - Code->VA, End - Start);
    if (Source != Emitted || Source.size() % aarch64::kInsnSize ||
        readLE<uint32_t>(Source.take_back(aarch64::kInsnSize).data()) !=
            kReturnToLR)
      return refuse(kLeafCode);
    for (size_t Offset = 0; Offset < Source.size() - aarch64::kInsnSize;
         Offset += aarch64::kInsnSize) {
      DecodedInsn Insn{};
      if (Decode.decodeOne(Source.data() + Offset, aarch64::kInsnSize,
                           Start + Offset, Insn) != aarch64::kInsnSize ||
          !Insn.Raw || !Insn.Raw->detail || !registerOnly(Insn))
        return refuse(kLeafCode);
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<PatchResult>
MachOPatcher::installLiftedLeaves(llvm::Module &Module,
                                  const BinaryImage &Image,
                                  const std::filesystem::path &Output) {
  auto Triple = sourcePatchTargetTriple(Image);
  if (!Triple)
    return Triple.takeError();
  if (!Module.getTargetTriple().isMacOSX() ||
      Module.getTargetTriple().getArch() != llvm::Triple::aarch64 ||
      !Module.global_empty() || !Module.alias_empty() ||
      !Module.ifunc_empty() || !Module.getModuleInlineAsm().empty() ||
      !Image.DataInCode.empty() || llvm::verifyModule(Module))
    return refuse(kLeafModule);
  const Section *Text = nullptr;
  for (const auto &S : Image.Sections) {
    if (!S.Size || !(S.Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                               llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)))
      continue;
    if (Text || S.Name != section_names::macho::Text ||
        S.SegmentName != section_names::macho::TextSeg ||
        !(S.Type & llvm::MachO::S_ATTR_PURE_INSTRUCTIONS) || S.FileSz != S.Size)
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kSourceSection, S.SegmentName, S.Name).str());
    Text = &S;
  }
  if (!Text || !Image.MachOHasFunctionStarts ||
      !rangeInBounds(Text->FileOff, Text->Size, Image.Raw.size()) ||
      llvm::any_of(Image.Segments, [](const auto &S) {
        return S.isExecutable() && S.Name != section_names::macho::TextSeg;
      }))
    return refuse(kSourceText);
  std::vector<SourcePatchMapping> Mappings;
  for (const auto &F : Module) {
    // Intrinsic declarations are not native entry points. Any machine code
    // their uses produce is checked with the rest of the emitted body.
    if (F.isDeclaration() && F.isIntrinsic())
      continue;
    auto Entry = rewrite_source::getOriginalVA(F);
    if (!Entry)
      return Entry.takeError();
    if (F.isDeclaration() || !*Entry || !Text->contains(**Entry) ||
        !Image.hasAuthenticatedFunctionEntryAt(**Entry))
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kSourceEntry, F.getName()).str());
    for (const auto &B : F)
      for (const auto &I : B)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            Call && Call->isInlineAsm())
          return refuse(kLeafModule);
    Mappings.push_back({F.getName().str(), **Entry});
  }
  llvm::sort(Mappings, [](const auto &L, const auto &R) {
    return L.OriginalVA < R.OriginalVA;
  });
  if (Mappings.empty() || Mappings.front().OriginalVA != Text->VA ||
      std::adjacent_find(Mappings.begin(), Mappings.end(),
                         [](const auto &L, const auto &R) {
                           return L.OriginalVA == R.OriginalVA;
                         }) != Mappings.end())
    return refuse(kLeafCoverage);

  const auto Missing = [&](va_t Address) {
    if (!Text->contains(Address))
      return false;
    const auto It = std::lower_bound(
        Mappings.begin(), Mappings.end(), Address,
        [](const SourcePatchMapping &M, va_t VA) { return M.OriginalVA < VA; });
    return It == Mappings.end() || It->OriginalVA != Address;
  };
  if ((Image.Entry && Missing(Image.Entry)) ||
      llvm::any_of(Image.Symbols,
                   [&](const auto &S) {
                     return S.IsFunc && !S.IsBoundaryGuess && Missing(S.Addr);
                   }) ||
      llvm::any_of(Image.Exports,
                   [&](const auto &E) { return Missing(E.Addr); }))
    return refuse(kLeafCoverage);

  // Only this private emission can consume the analysis marker. Its bytes go
  // directly to the transactional installer, which rechecks each relocated
  // machine body, its original identity and its actual unwind recipe.
  auto Candidate = llvm::CloneModule(Module);
  llvm::SmallVector<llvm::Module::ModuleFlagEntry> Flags;
  Candidate->getModuleFlagsMetadata(Flags);
  Candidate->getModuleFlagsMetadata()->clearOperands();
  for (const auto &Flag : Flags)
    if (Flag.Key->getString() != kArm64eAnalysisModuleFlag)
      Candidate->addModuleFlag(Flag.Behavior, Flag.Key->getString(), Flag.Val);
  auto &Context = Candidate->getContext();
  auto *Version = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
      llvm::Type::getInt32Ty(Context), kClassicABIVersion));
  auto *Kernel =
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getFalse(Context));
  auto *ABI = llvm::MDNode::get(Context, {Version, Kernel});
  auto *ExpectedFlag = llvm::MDNode::get(Context, {ABI});
  if (const auto *Existing = Candidate->getModuleFlag(kLeafABIFlag)) {
    if (Existing != ExpectedFlag)
      return refuse(kLeafModule);
  } else {
    Candidate->addModuleFlag(llvm::Module::AppendUnique, kLeafABIFlag,
                             ExpectedFlag);
  }
  auto Object =
      Codegen().compile(*Candidate, Image.Arch, Image.Format, *Triple);
  if (!Object.Success)
    return refuse(kLeafCode);
  return installMachOObjectPatch(
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()),
      Image, Mappings, {}, Output, {}, nullptr, nullptr, {},
      [&](const CompiledImage &Generated,
          llvm::ArrayRef<NativeMachOCompactRange> Ranges) {
        return verifyLeaves(Image, *Text, Mappings, Generated, Ranges);
      });
}
} // namespace neverd
