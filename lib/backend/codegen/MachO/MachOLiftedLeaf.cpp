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
#include "neverd/backend/llvm/AArch64/LLVMEntryABI.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd {
namespace {
#define MACHO_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#define MACHO_LEAF_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "MachOLiftedLeaf.def"

enum class SigningHint : uint32_t {
#define NEVERD_A64_PAUTH_HINT(Name, Word, Operation, Pointer, Modifier)        \
  Name = Word,
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.def"
};

llvm::Error refuse(const char *Reason) {
  return llvm::createStringError(llvm::errc::not_supported, Reason);
}

bool leafRegister(aarch64_reg R, bool ReadOnly) {
  switch (R) {
#define NEVERD_DARWIN_ENTRY_REGISTER(X, W, Name, Reservation, Writable)        \
  case AARCH64_REG_##X:                                                        \
  case AARCH64_REG_##W:                                                        \
    return ReadOnly || Writable;
#include "neverd/backend/llvm/AArch64/LLVMDarwinEntryRegister.def"
  default:
    break;
  }
  return (R >= AARCH64_REG_X0 && R <= AARCH64_REG_X17) ||
         (R >= AARCH64_REG_W0 && R <= AARCH64_REG_W17) ||
         (R >= AARCH64_REG_S0 && R <= AARCH64_REG_S7) ||
         (R >= AARCH64_REG_D0 && R <= AARCH64_REG_D7) || R == AARCH64_REG_XZR ||
         R == AARCH64_REG_WZR;
}

// MOVZ Wd/Xd, #0 and ORR Wd/Xd, ZR, ZR both clear the full destination
// register and preserve NZCV. The mask ignores only the width and Rd bits.
std::optional<unsigned> clearedRegister(uint32_t Word) {
  const unsigned Rd = Word & kDestinationMask;
  const uint32_t Operation = Word & kZeroOperationMask;
  if (Rd == kDestinationMask ||
      (Operation != kMoveWideZero && Operation != kMoveFromZero))
    return std::nullopt;
  return Rd;
}

bool equivalentLeafWord(uint32_t Original, uint32_t Emitted) {
  if (Original == Emitted)
    return true;
  const auto Rd = clearedRegister(Original);
  return Rd && Rd == clearedRegister(Emitted);
}

// Two adjacent zero writes to distinct GPRs have no input, flags, memory or
// unwind effects. Both source instructions still undergo the register checks.
bool exchangedZeroWrites(llvm::ArrayRef<uint8_t> Original,
                         llvm::ArrayRef<uint8_t> Emitted) {
  if (Original.size() < 2 * aarch64::kInsnSize)
    return false;
  const auto First = clearedRegister(readLE<uint32_t>(Original.data()));
  const auto Second =
      clearedRegister(readLE<uint32_t>(Original.data() + aarch64::kInsnSize));
  return First && Second && First != Second &&
         Second == clearedRegister(readLE<uint32_t>(Emitted.data())) &&
         First == clearedRegister(
                      readLE<uint32_t>(Emitted.data() + aarch64::kInsnSize));
}

bool isPositionIndependentLeafInstruction(const DecodedInsn &I) {
  const auto &Detail = I.Raw->detail->aarch64;
  switch (I.Id) {
#define MACHO_LEAF_INSTRUCTION(Name) case Name:
#include "MachOLiftedLeaf.def"
    return llvm::all_of(
        llvm::ArrayRef(Detail.operands, Detail.op_count),
        [](const cs_aarch64_op &Op) {
          return Op.type == AARCH64_OP_IMM ||
                 (Op.type == AARCH64_OP_REG &&
                  leafRegister(Op.reg, Op.access == CS_AC_READ));
        });
#define MACHO_LEAF_MEMORY(Name) case Name:
#include "MachOLiftedLeaf.def"
    // A literal LDR has the same instruction ID. Insist on an actual base
    // register and displacement, with neither PC-relative addressing nor
    // an implicit write to the base.
    return Detail.op_count == kMemoryOperandCount &&
           !I.Raw->detail->writeback && !Detail.post_index &&
           Detail.operands[0].type == AARCH64_OP_REG &&
           leafRegister(Detail.operands[0].reg,
                        Detail.operands[0].access == CS_AC_READ) &&
           Detail.operands[1].type == AARCH64_OP_MEM &&
           Detail.operands[1].mem.index == AARCH64_REG_INVALID &&
           leafRegister(Detail.operands[1].mem.base, true);
  default:
    return false;
  }
}

bool matchesTailTransfer(llvm::ArrayRef<uint8_t> Source, va_t OriginalPC,
                         llvm::ArrayRef<uint8_t> Installed, va_t InstalledPC,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         const CompiledImage &Generated,
                         const BinaryImage &Image, va_t Slide) {
  const auto Target = [](llvm::ArrayRef<uint8_t> Word, va_t PC) {
    auto Jump =
        decodeDirectBranch(Arch::AArch64, InstructionMode::Default, Word.data(),
                           Word.size(), PC, {.Jumps = true});
    return Jump ? Jump->target() : std::nullopt;
  };
  const auto Before = Target(Source, OriginalPC);
  const auto After = Target(Installed, InstalledPC);
  if (!Before || !After)
    return false;
  if (Image.findImportStubAt(*Before))
    return *After == *Before + Slide;
  const auto Callee = llvm::lower_bound(
      Mappings, *Before,
      [](const SourcePatchMapping &M, va_t VA) { return M.OriginalVA < VA; });
  if (Callee == Mappings.end() || Callee->OriginalVA != *Before)
    return false;
  const auto Owner = Generated.SymbolAddrs.find(Callee->Symbol);
  return Owner != Generated.SymbolAddrs.end() && Owner->second == *After;
}

// Darwin's ordinary calling convention restores X21 in the epilogue. A leaf
// with an observable X21 write needs the target's writable error carrier.
llvm::Error bindLeafErrorResults(llvm::Module &Module) {
  auto &Context = Module.getContext();
  auto *Register = llvm::MetadataAsValue::get(
      Context, llvm::MDNode::get(Context, llvm::MDString::get(
                                              Context, kDarwinErrorRegister)));
  llvm::SetVector<llvm::Function *> Writers;
  llvm::DenseMap<llvm::Function *, llvm::SmallVector<llvm::IntrinsicInst *>>
      Accesses;
  for (auto &F : Module) {
    for (auto &BB : F)
      for (auto &I : BB) {
        auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        if (!Intrinsic)
          continue;
        const auto ID = Intrinsic->getIntrinsicID();
        if ((ID == llvm::Intrinsic::read_register ||
             ID == llvm::Intrinsic::write_register) &&
            Intrinsic->getArgOperand(0) == Register) {
          if (ID == llvm::Intrinsic::write_register)
            Writers.insert(&F);
          Accesses[&F].push_back(Intrinsic);
        }
      }
  }
  // A direct tail caller shares its caller's X21 with the writer. Discover
  // the entire chain before replacing any prototypes, including recursion.
  for (size_t I = 0; I < Writers.size(); ++I)
    for (auto &Use : Writers[I]->uses()) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(Use.getUser());
      if (!Call || !Call->isCallee(&Use) || !Call->isTailCall() ||
          Call->hasOperandBundles())
        return refuse(kErrorResultABI);
      Writers.insert(Call->getFunction());
    }
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  llvm::DenseMap<llvm::Function *, llvm::Function *> Entries;
  for (auto *Old : Writers) {
    auto &F = *Old;
    if (F.isVarArg() || F.getCallingConv() != llvm::CallingConv::C)
      return refuse(kErrorResultABI);
    auto *OldType = F.getFunctionType();
    llvm::SmallVector<llvm::Type *> Params(OldType->params());
    Params.push_back(Pointer);
    auto *Type =
        llvm::FunctionType::get(OldType->getReturnType(), Params, false);
    auto *Entry = llvm::Function::Create(Type, F.getLinkage(),
                                         F.getAddressSpace(), {}, &Module);
    Module.getFunctionList().splice(F.getIterator(), Module.getFunctionList(),
                                    Entry->getIterator());
    Entry->copyAttributesFrom(&F);
    Entry->copyMetadata(&F, 0);
    Entry->takeName(&F);
    Entry->setCallingConv(llvm::CallingConv::Swift);
    Entry->removeFnAttr(llvm::Attribute::Memory);
    auto *Error = Entry->getArg(F.arg_size());
    Error->addAttr(llvm::Attribute::SwiftError);
    for (auto &Old : F.args()) {
      auto *New = Entry->getArg(Old.getArgNo());
      New->takeName(&Old);
      Old.replaceAllUsesWith(New);
    }
    Entry->splice(Entry->begin(), &F);
    Entries[Old] = Entry;
  }
  for (auto *Old : Writers) {
    auto *Entry = Entries.lookup(Old);
    auto *Error = Entry->getArg(Old->arg_size());
    for (auto &BB : *Entry)
      for (auto &I : llvm::make_early_inc_range(BB)) {
        auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call || llvm::isa<llvm::IntrinsicInst>(Call))
          continue;
        auto *Callee = Entries.lookup(Call->getCalledFunction());
        if (!Callee)
          return refuse(kErrorResultABI);
        auto *Tail = llvm::cast<llvm::CallInst>(Call);
        llvm::SmallVector<llvm::Value *> Args(Tail->args());
        Args.push_back(Error);
        auto *Forward = llvm::IRBuilder<>(Tail).CreateCall(Callee, Args);
        Forward->copyMetadata(*Tail);
        Forward->setCallingConv(llvm::CallingConv::Swift);
        Forward->setTailCallKind(Tail->getTailCallKind());
        Forward->setAttributes(Tail->getAttributes());
        Forward->removeFnAttr(llvm::Attribute::Memory);
        Forward->addParamAttr(Callee->arg_size() - 1,
                              llvm::Attribute::SwiftError);
        Forward->takeName(Tail);
        Tail->replaceAllUsesWith(Forward);
        Tail->eraseFromParent();
      }
    for (auto *Access : Accesses[Old]) {
      llvm::IRBuilder<> B(Access);
      if (Access->getIntrinsicID() == llvm::Intrinsic::read_register) {
        Access->replaceAllUsesWith(
            B.CreatePtrToInt(B.CreateLoad(Pointer, Error), Access->getType()));
      } else {
        B.CreateStore(B.CreateIntToPtr(Access->getArgOperand(1), Pointer),
                      Error);
      }
      Access->eraseFromParent();
    }
  }
  for (auto *Old : Writers) {
    Old->replaceAllUsesWith(Entries.lookup(Old));
    Old->eraseFromParent();
  }
  return llvm::Error::success();
}

llvm::Error verifyLeaves(const BinaryImage &Original, const Section &Text,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         const CompiledImage &Generated, va_t Slide,
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
    if (Source.empty() || Source.size() % aarch64::kInsnSize)
      return refuse(kLeafCode);
    bool Signed = false;
    size_t MatchedEnd = 0;
    for (size_t Offset = 0; Offset < Source.size();
         Offset += aarch64::kInsnSize) {
      DecodedInsn Insn{};
      if (Decode.decodeOne(Source.data() + Offset, aarch64::kInsnSize,
                           Start + Offset, Insn) != aarch64::kInsnSize ||
          !Insn.Raw || !Insn.Raw->detail)
        return refuse(kLeafCode);
      const bool Last = Offset + aarch64::kInsnSize == Source.size();
      if (!Last && Insn.Id == AARCH64_INS_ADRP) {
        // The linked instruction has a new PC. Its destination register
        // and the original data page it materializes must stay identical.
        const auto &SourceAdr = Insn.Raw->detail->aarch64;
        const auto Dest = SourceAdr.operands[0].reg;
        const va_t Page = SourceAdr.operands[1].imm;
        const bool DataPage =
            llvm::any_of(Original.Sections, [&](const auto &S) {
              return S.Size && !S.isExecutable() &&
                     (S.contains(Page) ||
                      llvm::alignDown(S.VA, kInstructionPageBytes) == Page);
            });
        DecodedInsn Adr{};
        if (!DataPage || !leafRegister(Dest, false) ||
            Decode.decodeOne(Emitted.data() + Offset, aarch64::kInsnSize,
                             Owner->second + Offset,
                             Adr) != aarch64::kInsnSize ||
            Adr.Id != AARCH64_INS_ADRP || !Adr.Raw || !Adr.Raw->detail)
          return refuse(kDataPage);
        const auto &Result = Adr.Raw->detail->aarch64;
        if (Result.operands[0].reg != Dest ||
            va_t(Result.operands[1].imm) != Page + Slide)
          return refuse(kDataPage);
        continue;
      }
      if (Last && !Signed && Insn.Id == AARCH64_INS_B) {
        if (!matchesTailTransfer(Source.drop_front(Offset), Start + Offset,
                                 Emitted.drop_front(Offset),
                                 Owner->second + Offset, Mappings, Generated,
                                 Original, Slide))
          return refuse(kTailTarget);
        continue;
      }
      if (Offset >= MatchedEnd &&
          !equivalentLeafWord(readLE<uint32_t>(Source.data() + Offset),
                              readLE<uint32_t>(Emitted.data() + Offset))) {
        if (!exchangedZeroWrites(Source.drop_front(Offset),
                                 Emitted.drop_front(Offset)))
          return refuse(kLeafCode);
        MatchedEnd = Offset + 2 * aarch64::kInsnSize;
      }
      if (Last) {
        // Plain RET must select LR. RETAB has an implicit LR, SP and B key.
        if (Signed ? Insn.Id != AARCH64_INS_RETAB
                   : readLE<uint32_t>(Source.data() + Offset) != kReturnToLR)
          return refuse(kLeafCode);
      } else if (!Offset && readLE<uint32_t>(Source.data()) ==
                                static_cast<uint32_t>(SigningHint::PACIBSP)) {
        Signed = true;
      } else if (!isPositionIndependentLeafInstruction(Insn)) {
        return refuse(kLeafCode);
      }
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
      !Module.alias_empty() || !Module.ifunc_empty() ||
      !Module.getModuleInlineAsm().empty() || !Image.DataInCode.empty() ||
      llvm::verifyModule(Module))
    return refuse(kLeafModule);
  const Section *Text = nullptr;
  for (const auto &S : Image.Sections) {
    if (!S.Size || !(S.Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                               llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)))
      continue;
    if (S.SegmentName == section_names::macho::TextSeg &&
        (S.Type & llvm::MachO::SECTION_TYPE) == llvm::MachO::S_SYMBOL_STUBS)
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
  std::vector<SourcePatchBinding> Bindings;
  for (const auto &F : Module) {
    // Intrinsic declarations are not native entry points. Any machine code
    // their uses produce is checked with the rest of the emitted body.
    if (F.isDeclaration() && F.isIntrinsic())
      continue;
    auto Entry = rewrite_source::getOriginalVA(F);
    if (!Entry)
      return Entry.takeError();
    if (F.isDeclaration() && *Entry && Image.findImportStubAt(**Entry)) {
      if (!F.use_empty())
        Bindings.push_back(
            {F.getName().str(), SourcePatchBindingKind::Function, **Entry, 0});
      continue;
    }
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
  // Data mirrors describe original storage. Their LLVM array extent can
  // cover the page padding beyond the symbol. Bind only the address used in
  // relocations. Each emitted load/store and displacement is checked below.
  for (auto &GV : llvm::make_early_inc_range(Candidate->globals())) {
    auto Address = parseNdDataSymbol(GV.getName());
    if (!Address || GV.isThreadLocal() || GV.getAddressSpace())
      return refuse(kDataBinding);
    if (GV.use_empty()) {
      GV.eraseFromParent();
      continue;
    }
    GV.setInitializer(nullptr);
    GV.setLinkage(llvm::GlobalValue::ExternalLinkage);
    GV.setDSOLocal(true);
    Bindings.push_back({GV.getName().str(),
                        SourcePatchBindingKind::ReadOnlyData, *Address,
                        sizeof(uint8_t)});
  }
  llvm::SmallVector<llvm::Module::ModuleFlagEntry> Flags;
  Candidate->getModuleFlagsMetadata(Flags);
  Candidate->getModuleFlagsMetadata()->clearOperands();
  for (const auto &Flag : Flags)
    if (Flag.Key->getString() != kArm64eAnalysisModuleFlag)
      Candidate->addModuleFlag(Flag.Behavior, Flag.Key->getString(), Flag.Val);
  if (auto Error = bindLeafErrorResults(*Candidate))
    return Error;
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
      Image, Mappings, Bindings, Output, {}, nullptr, nullptr, {},
      [&](const CompiledImage &Generated, va_t Slide,
          llvm::ArrayRef<NativeMachOCompactRange> Ranges) {
        return verifyLeaves(Image, *Text, Mappings, Generated, Slide, Ranges);
      });
}
} // namespace neverd
