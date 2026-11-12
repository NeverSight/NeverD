//===- MachOLiftedLeaf.cpp - Verified Darwin register-only leaf images
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOLiftedObject.h"

#include "neverd/analysis/LLVMPrivateFrameAccesses.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/MachO/MachOVariadicRegisters.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/backend/llvm/AArch64/LLVMEntryABI.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <tuple>

namespace neverd {
namespace {
#define MACHO_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#define MACHO_LEAF_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_LEAF_SEQUENCE(Name, ...)                                         \
  constexpr uint32_t Name[] = {__VA_ARGS__};
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
           (Detail.operands[1].mem.base == AARCH64_REG_SP ||
            leafRegister(Detail.operands[1].mem.base, true));
  default:
    return false;
  }
}

// A frameless leaf may reserve stack space that it never accesses. Removing
// a matched SP subtraction/addition preserves its register and memory effects
// if every load still reads the same caller byte with the same instruction
// form. The installed leaf then has its own zero-frame compact unwind row.
bool elidesUnusedStackFrame(Decoder &Decode, llvm::ArrayRef<uint8_t> Source,
                            llvm::ArrayRef<uint8_t> Emitted, va_t OldVA,
                            va_t NewVA, const CompactUnwindEntry &Frame) {
  constexpr unsigned Word = aarch64::kInsnSize;
  if (!Frame.HasStackSize || !Frame.StackSize ||
      (Frame.NativeEncoding & ~macho_unwind::kARM64FramelessStackSizeMask) !=
          macho_unwind::kARM64ModeFrameless ||
      Emitted.size() < Word || Emitted.size() % Word ||
      Source.size() <= Emitted.size() ||
      readLE<uint32_t>(Source.end() - Word) != kReturnToLR ||
      readLE<uint32_t>(Emitted.end() - Word) != kReturnToLR)
    return false;
  const auto Read = [&](llvm::ArrayRef<uint8_t> Bytes, va_t VA, size_t Offset,
                        DecodedInsn &I) {
    return Decode.decodeOne(Bytes.data() + Offset, Word, VA + Offset, I) ==
               Word &&
           I.Raw && I.Raw->detail;
  };
  const auto AdjustsSP = [&](size_t Offset, unsigned Opcode) {
    DecodedInsn I{};
    if (!Read(Source, OldVA, Offset, I) || I.Id != Opcode)
      return false;
    const auto &D = I.Raw->detail->aarch64;
    return D.op_count == kArithmeticOperandCount &&
           D.operands[0].type == AARCH64_OP_REG &&
           D.operands[0].reg == AARCH64_REG_SP &&
           D.operands[1].type == AARCH64_OP_REG &&
           D.operands[1].reg == AARCH64_REG_SP &&
           D.operands[2].type == AARCH64_OP_IMM && !D.operands[2].shift.value &&
           D.operands[2].imm == Frame.StackSize;
  };
  // Clang may restore SP with a pre-indexed load instead of a separate ADD.
  const size_t Removed = Source.size() - Emitted.size();
  const bool SeparateRestore = Removed == 2 * Word;
  if ((!SeparateRestore && Removed != Word) || !AdjustsSP(0, AARCH64_INS_SUB) ||
      (SeparateRestore &&
       !AdjustsSP(Source.size() - 2 * Word, AARCH64_INS_ADD)))
    return false;

  const auto StackLoad = [](const DecodedInsn &I)
      -> std::optional<std::pair<aarch64_reg, int64_t>> {
    const auto &D = I.Raw->detail->aarch64;
    if (I.Id != AARCH64_INS_LDR || !isPositionIndependentLeafInstruction(I) ||
        D.operands[1].mem.base != AARCH64_REG_SP)
      return std::nullopt;
    return {{D.operands[0].reg, D.operands[1].mem.disp}};
  };
  int64_t Depth = Frame.StackSize;
  for (size_t Off = 0; Off < Emitted.size() - Word; Off += Word) {
    const size_t Old = Off + Word;
    DecodedInsn I{};
    if (!Read(Source, OldVA, Old, I))
      return false;
    auto Load = StackLoad(I);
    if (I.Id == AARCH64_INS_LDR && I.Raw->detail->writeback) {
      const auto Update = aarch64WritebackMemoryInstruction(
          readLE<uint32_t>(Source.data() + Old));
      if (SeparateRestore || !Depth || !Update || !Update->stackBase() ||
          !Update->preIndex() || Update->displacement() != uint64_t(Depth))
        return false;
      // Pre-indexing accesses the updated base. Both the load and every
      // subsequent SP-relative access now use the incoming SP.
      Depth = 0;
      Load = {{I.Raw->detail->aarch64.operands[0].reg, 0}};
    }
    if (Load) {
      // Decode borrows its instruction until the next decode.
      if (Load->second < Depth || !Read(Emitted, NewVA, Off, I))
        return false;
      Load->second -= Depth;
      if (StackLoad(I) != Load)
        return false;
      continue;
    }
    if (I.Id == AARCH64_INS_LDR || I.Id == AARCH64_INS_STR ||
        !isPositionIndependentLeafInstruction(I) ||
        readLE<uint32_t>(Source.data() + Old) !=
            readLE<uint32_t>(Emitted.data() + Off))
      return false;
  }
  return SeparateRestore || Depth == 0;
}

bool matchesDirectTransfer(llvm::ArrayRef<uint8_t> Source, va_t OriginalPC,
                           llvm::ArrayRef<uint8_t> Installed, va_t InstalledPC,
                           llvm::ArrayRef<SourcePatchMapping> Mappings,
                           const CompiledImage &Generated,
                           const BinaryImage &Image, va_t Slide,
                           DirectBranchForms Forms) {
  const auto Target = [Forms](llvm::ArrayRef<uint8_t> Word, va_t PC) {
    auto Jump = decodeDirectBranch(Arch::AArch64, InstructionMode::Default,
                                   Word.data(), Word.size(), PC, Forms);
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

// LLVM lowers a swiftcc call with a writable error result as a call and
// return. A transparent forwarding entry can transfer every incoming
// register, including X21 and LR, directly to the callee. Emit the native
// branch with a symbol relocation and check its final target in verifyLeaves.
void emitSwiftErrorForwarder(llvm::Function &Entry) {
  if (Entry.size() != 1 || Entry.front().size() != 2)
    return;
  auto *Call = llvm::dyn_cast<llvm::CallInst>(&Entry.front().front());
  auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Entry.front().getTerminator());
  if (!Call || !Ret || !Call->isTailCall() || !Call->getCalledFunction() ||
      Call->getCallingConv() != llvm::CallingConv::Swift ||
      Call->getFunctionType() != Entry.getFunctionType() ||
      (Call->getType()->isVoidTy() ? Ret->getReturnValue() != nullptr
                                   : Ret->getReturnValue() != Call))
    return;
  for (auto [I, Argument] : llvm::enumerate(Entry.args()))
    if (Call->getArgOperand(I) != &Argument)
      return;
  llvm::IRBuilder<> B(Call);
  auto *Target = Call->getCalledFunction();
  auto *Ty = llvm::FunctionType::get(B.getVoidTy(), {Target->getType()}, false);
  auto *Asm = llvm::InlineAsm::get(Ty, kTailBranchAsm, kTailBranchSymbol, true);
  B.CreateCall(Asm, {Target})->setDoesNotReturn();
  B.CreateUnreachable();
  Ret->eraseFromParent();
  Call->eraseFromParent();
  Entry.addFnAttr(llvm::Attribute::Naked);
  Entry.setUWTableKind(llvm::UWTableKind::Sync);
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
  // Follow direct callers before replacing any prototypes, including
  // recursion. The native verifier checks each call and its frame.
  for (size_t I = 0; I < Writers.size(); ++I)
    for (auto &Use : Writers[I]->uses()) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(Use.getUser());
      if (!Call || !Call->isCallee(&Use) || Call->hasOperandBundles())
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
        auto *Direct = llvm::cast<llvm::CallInst>(Call);
        llvm::IRBuilder<> B(Direct);
        llvm::Value *CallError = Error;
        if (!Direct->isTailCall()) {
          // Clang gives a call with a continuation a local swifterror slot.
          // Copy in the incoming X21 and propagate the callee's value back.
          llvm::IRBuilder<> Prologue(
              &*Entry->getEntryBlock().getFirstInsertionPt());
          auto *Local = Prologue.CreateAlloca(Pointer);
          Local->setSwiftError(true);
          B.CreateStore(B.CreateLoad(Pointer, Error), Local);
          CallError = Local;
        }
        llvm::SmallVector<llvm::Value *> Args(Direct->args());
        Args.push_back(CallError);
        auto *Forward = B.CreateCall(Callee, Args);
        if (CallError != Error)
          B.CreateStore(B.CreateLoad(Pointer, CallError), Error);
        Forward->copyMetadata(*Direct);
        Forward->setCallingConv(llvm::CallingConv::Swift);
        Forward->setTailCallKind(Direct->getTailCallKind());
        Forward->setAttributes(Direct->getAttributes());
        Forward->removeFnAttr(llvm::Attribute::Memory);
        Forward->addParamAttr(Callee->arg_size() - 1,
                              llvm::Attribute::SwiftError);
        Forward->takeName(Direct);
        Direct->replaceAllUsesWith(Forward);
        Direct->eraseFromParent();
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
    emitSwiftErrorForwarder(*Entries.lookup(Old));
    Old->replaceAllUsesWith(Entries.lookup(Old));
    Old->eraseFromParent();
  }
  return llvm::Error::success();
}

struct FrameBody {
  size_t Begin;
  size_t End;
};

std::optional<FrameBody> signedFrameBody(llvm::ArrayRef<uint8_t> Code,
                                         llvm::ArrayRef<uint32_t> Save,
                                         llvm::ArrayRef<uint32_t> Restore) {
  constexpr auto Size = aarch64::kInsnSize;
  if (Code.size() / Size < Save.size() + Restore.size() + 2 ||
      readLE<uint32_t>(Code.data()) !=
          static_cast<uint32_t>(SigningHint::PACIBSP))
    return std::nullopt;
  const size_t End = Code.size() - (Restore.size() + 1) * Size;
  for (auto [Index, Word] : llvm::enumerate(Save))
    if (readLE<uint32_t>(Code.data() + Size * (Index + 1)) != Word)
      return std::nullopt;
  for (auto [Index, Word] : llvm::enumerate(Restore))
    if (readLE<uint32_t>(Code.data() + End + Size * Index) != Word)
      return std::nullopt;
  return FrameBody{(Save.size() + 1) * Size, End};
}

// Frame instructions materialize CodeOffset relative to each function.
// Padding NOPs can change when the linker lays out the relocated records.
bool sameSpillRules(llvm::ArrayRef<CFIInstruction> Old,
                    llvm::ArrayRef<CFIInstruction> New) {
  const auto Effective = [](const CFIInstruction &I) {
    return I.Kind != CFIOpKind::Nop;
  };
  auto Before = llvm::make_filter_range(Old, Effective);
  auto After = llvm::make_filter_range(New, Effective);
  return llvm::all_of(Before,
                      [](const auto &I) {
                        switch (I.Kind) {
                        case CFIOpKind::AdvanceLoc:
                        case CFIOpKind::DefCFA:
                        case CFIOpKind::DefCFAOffset:
                        case CFIOpKind::Offset:
                        case CFIOpKind::Restore:
                          return true;
                        default:
                          return false;
                        }
                      }) &&
         llvm::equal(Before, After, dwarf_eh::sameCFIInstruction);
}

// The compact table points to a specific FDE. Compare the compiler's LR
// spill and its return rules at every original instruction boundary.
bool sameIndexedLinkFrame(const BinaryImage &Original,
                          const ExceptionFunction &Frame,
                          const dwarf_eh::ParseResult &Generated,
                          const DwarfFDE *NewFrame, va_t Start,
                          const NativeMachOCompactRange &Range) {
  if (!Frame.Dwarf || !NewFrame || Frame.Dwarf->InitialLocation != Start ||
      Frame.Dwarf->AddressRange != Range.EndVA - Range.BeginVA ||
      Frame.Dwarf->LSDAVA ||
      Frame.Dwarf->ParseStatus != ExceptionParseStatus::Complete ||
      (Frame.Compact->NativeEncoding & macho_unwind::kDwarfSectionOffsetMask) !=
          Frame.Dwarf->SectionOffset ||
      Generated.ParseStatus != ExceptionParseStatus::Complete)
    return false;
  const auto *Old =
      Original.ExceptionMetadata.findCIE(Frame.Dwarf->CIESectionOffset);
  if (!Old || Old->Augmentation != kCompilerFrameAugmentation ||
      Old->ParseStatus != ExceptionParseStatus::Complete || NewFrame->LSDAVA ||
      NewFrame->AddressRange != Frame.Dwarf->AddressRange ||
      NewFrame->ParseStatus != ExceptionParseStatus::Complete)
    return false;
  const auto New = llvm::lower_bound(
      Generated.CIEs, NewFrame->CIESectionOffset,
      [](const DwarfCIE &C, uint64_t Off) { return C.SectionOffset < Off; });
  const auto ABI = [](const DwarfCIE &C) {
    return std::tie(C.Version, C.Augmentation, C.CodeAlignmentFactor,
                    C.DataAlignmentFactor, C.ReturnAddressRegister,
                    C.AddressSize, C.SegmentSelectorSize, C.FDEPointerEncoding,
                    C.UsesArm64eReturnAddressABI, C.ParseStatus);
  };
  return New != Generated.CIEs.end() &&
         New->SectionOffset == NewFrame->CIESectionOffset &&
         ABI(*Old) == ABI(*New) &&
         sameSpillRules(Old->InitialInstructions, New->InitialInstructions) &&
         sameSpillRules(Frame.Dwarf->Instructions, NewFrame->Instructions);
}

llvm::Error verifyLeaves(const BinaryImage &Original, const Section &Text,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         const CompiledImage &Generated, va_t Slide,
                         llvm::ArrayRef<NativeMachOCompactRange> Ranges,
                         const llvm::DenseSet<va_t> &RegisterEntries) {
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  const CompiledSection *Code = nullptr;
  dwarf_eh::ParseResult LinkedFrames;
  for (const auto &Section : Generated.Sections) {
    if (Section.Kind != Kind::Code) {
      if (Section.Name == section_names::macho::EhFrame &&
          !Section.ExternalBytes.empty())
        LinkedFrames =
            dwarf_eh::parseEHFrame(Original,
                                   {Section.ExternalBytes.data(),
                                    Section.ExternalBytes.size(), Section.VA},
                                   {});
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
  llvm::DenseMap<va_t, const DwarfFDE *> FDEs;
  for (const auto &F : LinkedFrames.FDEs)
    if (!FDEs.try_emplace(F.InitialLocation, &F).second)
      return refuse(kLeafFrame);
  if (!Code || Ranges.size() != Mappings.size())
    return llvm::createStringError(
        llvm::errc::not_supported,
        llvm::formatv(kGeneratedCoverage, Ranges.size(), Mappings.size(),
                      Code ? Code->Size : 0, Text.Size)
            .str());
  std::map<va_t, const NativeMachOCompactRange *> ByAddress;
  for (const auto &R : Ranges)
    if (!ByAddress.emplace(R.BeginVA, &R).second)
      return refuse(kLeafCoverage);

  uint64_t Covered = 0;
  for (const auto &[Start, Range] : ByAddress) {
    if (Start < Code->VA || Start - Code->VA != Covered ||
        Range->EndVA <= Start ||
        !rangeInBounds(Covered, Range->EndVA - Start, Code->Size))
      return refuse(kLeafCoverage);
    Covered += Range->EndVA - Start;
  }
  if (Covered != Code->Size)
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
    if (Range.empty())
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kGeneratedRange, Mapping.Symbol, Start, End).str());
    if (!Frame || !Frame->describesUnwindOnly() || !Frame->Compact ||
        Frame->CodeRange.End < End)
      return refuse(kLeafFrame);
    const auto Native = Range.mapped()->Encoding;
    const auto Source = llvm::ArrayRef(Original.Raw)
                            .slice(Text.FileOff + Start - Text.VA, End - Start);
    const auto Emitted = llvm::ArrayRef(Generated.Bytes)
                             .slice(Code->Offset + (Owner->second - Code->VA),
                                    Range.mapped()->EndVA - Owner->second);
    if (Source.empty() || Source.size() % aarch64::kInsnSize)
      return refuse(kLeafCode);
    if (!RegisterEntries.contains(Start) &&
        Native == macho_unwind::kARM64ModeFrameless &&
        Source.size() > Emitted.size() &&
        elidesUnusedStackFrame(Decode, Source, Emitted, Start, Owner->second,
                               *Frame->Compact)) {
      if (llvm::any_of(Original.ExceptionMetadata.Functions,
                       [&](const auto &F) {
                         return F.Dwarf && F.CodeRange.Begin < End &&
                                F.CodeRange.End > Start;
                       }))
        return refuse(kLeafFrame);
      continue;
    }
    if (Source.size() != Emitted.size())
      return refuse(kLeafCoverage);
    const bool Indexed = (Native & ~macho_unwind::kDwarfSectionOffsetMask) ==
                         macho_unwind::kARM64ModeDwarf;
    const auto SourceEncoding = Frame->Compact->NativeEncoding;
    if (Indexed) {
      if ((SourceEncoding & ~macho_unwind::kDwarfSectionOffsetMask) !=
              macho_unwind::kARM64ModeDwarf ||
          !sameIndexedLinkFrame(Original, *Frame, LinkedFrames,
                                FDEs.lookup(Owner->second), Start,
                                *Range.mapped()))
        return refuse(kLeafFrame);
    } else if (SourceEncoding != Native) {
      return refuse(kLeafFrame);
    }

    if (RegisterEntries.contains(Start)) {
      // The register lowering emits one instruction at each original offset.
      // A local branch therefore has the same displacement. Calls cross entry
      // boundaries and must resolve to the new symbol after final placement.
      if (!Indexed && (FDEs.count(Owner->second) || Frame->Dwarf))
        return refuse(kLeafFrame);
      for (size_t Offset = 0; Offset < Source.size();
           Offset += aarch64::kInsnSize) {
        auto Before = Source.drop_front(Offset);
        auto After = Emitted.drop_front(Offset);
        if (decodeDirectBranch(Original.Arch, InstructionMode::Default,
                               Before.data(), Before.size(), Start + Offset,
                               {.Jumps = false, .Calls = true})) {
          if (!matchesDirectTransfer(Before, Start + Offset, After,
                                     Owner->second + Offset, Mappings,
                                     Generated, Original, Slide,
                                     {.Jumps = false, .Calls = true}))
            return refuse(kDirectTarget);
        } else if (!equivalentLeafWord(readLE<uint32_t>(Before.data()),
                                       readLE<uint32_t>(After.data()))) {
          return refuse(kLeafCode);
        }
      }
      continue;
    }

    const bool Framed =
        Range.mapped()->Encoding == macho_unwind::kARM64ModeFrame;
    std::optional<FrameBody> Body;
    if (Framed) {
      Body = signedFrameBody(Source, kPushFrame, kPopFrame);
      if (!Body)
        Body = signedFrameBody(Source, kAllocateFrame, kDeallocateFrame);
      if (!Body)
        return refuse(kLeafFrame);
    } else if (Indexed) {
      Body = signedFrameBody(Source, kPushLink, kPopLink);
      if (!Body)
        return refuse(kLeafFrame);
    } else if (Native != macho_unwind::kARM64ModeFrameless) {
      return refuse(kLeafFrame);
    }
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
      const bool Call = Body && Offset >= Body->Begin && Offset < Body->End &&
                        Insn.Id == AARCH64_INS_BL;
      if (Call || (Last && !Signed && Insn.Id == AARCH64_INS_B)) {
        if (!matchesDirectTransfer(
                Source.drop_front(Offset), Start + Offset,
                Emitted.drop_front(Offset), Owner->second + Offset, Mappings,
                Generated, Original, Slide, {.Jumps = !Call, .Calls = Call}))
          return refuse(kDirectTarget);
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
      } else if (Body && (Offset < Body->Begin || Offset >= Body->End)) {
        // Both images performed the same frame operation at this offset.
        continue;
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
  auto RegisterEntries = validateMachOVariadicRegisters(Module, Image);
  if (!RegisterEntries)
    return RegisterEntries.takeError();
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
            Call && Call->isInlineAsm() && !RegisterEntries->contains(**Entry))
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
  for (const auto &Mapping : Mappings) {
    const auto *Frame =
        Image.ExceptionMetadata.findFunction(Mapping.OriginalVA);
    if (!Frame || !Frame->Compact)
      continue;
    auto *Entry = Candidate->getFunction(Mapping.Symbol);
    if (Frame->Dwarf &&
        (Frame->Compact->NativeEncoding & macho_unwind::kModeMask) ==
            macho_unwind::kARM64ModeDwarf) {
      const auto &FDE = *Frame->Dwarf;
      const auto *CIE = Image.ExceptionMetadata.findCIE(FDE.CIESectionOffset);
      if (!CIE)
        return refuse(kLeafFrame);
      // Synchronous Clang records can end at the call-site row. Preserve
      // an epilogue row when the source actually describes the restored LR.
      const bool HasEpilogue =
          llvm::any_of(FDE.Instructions, [&](const auto &I) {
            return I.Kind == CFIOpKind::Restore &&
                   I.Register == CIE->ReturnAddressRegister &&
                   FDE.AddressRange >= aarch64::kInsnSize &&
                   I.CodeOffset == FDE.AddressRange - aarch64::kInsnSize;
          });
      Entry->setUWTableKind(HasEpilogue ? llvm::UWTableKind::Async
                                        : llvm::UWTableKind::Sync);
      continue;
    }
    if (Frame->Compact->NativeEncoding != macho_unwind::kARM64ModeFrame)
      continue;
    // The source supplies a compact X29 record. An additional FDE may
    // specify distinct asynchronous locations and needs its own replay.
    if (llvm::any_of(Image.ExceptionMetadata.Functions, [&](const auto &F) {
          return F.Dwarf && F.CodeRange.contains(Mapping.OriginalVA);
        }))
      return refuse(kLeafFrame);
    Entry->addFnAttr(kFramePointerAttribute, kNonLeafFrame);
    Entry->setUWTableKind(llvm::UWTableKind::Sync);
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
  // The native prologue saves FP/LR itself. Remove only unobservable
  // accesses to the lifter's private frame before emitting that prologue.
  canonicalizePrivateFrameAccesses(*Candidate);
  {
    llvm::FunctionAnalysisManager Analyses;
    llvm::PassBuilder().registerFunctionAnalyses(Analyses);
    llvm::FunctionPassManager Cleanup;
    Cleanup.addPass(llvm::DCEPass());
    Cleanup.addPass(llvm::SROAPass(llvm::SROAOptions::PreserveCFG));
    Cleanup.addPass(llvm::DCEPass());
    for (auto &F : *Candidate)
      if (!F.isDeclaration())
        Cleanup.run(F, Analyses);
  }
  auto Object =
      Codegen().compile(*Candidate, Image.Arch, Image.Format, *Triple);
  if (!Object.Success)
    return refuse(kLeafCode);
  return installMachOLiftedObject(
      llvm::StringRef(reinterpret_cast<const char *>(Object.ObjectData.data()),
                      Object.ObjectData.size()),
      Image, Mappings, Bindings, Output,
      [&](const CompiledImage &Generated, va_t Slide,
          llvm::ArrayRef<NativeMachOCompactRange> Ranges) {
        return verifyLeaves(Image, *Text, Mappings, Generated, Slide, Ranges,
                            *RegisterEntries);
      });
}
} // namespace neverd
