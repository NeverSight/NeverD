//===- MachOLiftedLeaf.cpp - Verified Darwin register-only leaf images
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOArm64eABI.h"
#include "MachOLiftedObject.h"
#include "MachOPageAddress.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/analysis/LLVMPrivateFrameAccesses.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/MachO/MachORegisterResults.h"
#include "neverd/backend/codegen/SourcePatch.h"
#include "neverd/backend/llvm/AArch64/LLVMEntryABI.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/FunctionBody.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/AArch64Literal.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

#include <climits>
#include <iterator>
#include <map>
#include <tuple>

namespace neverd {
namespace {
#define MACHO_LEAF_TEXT(Name, Text) constexpr char Name[] = Text;
#define MACHO_LEAF_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_LEAF_SEQUENCE(Name, ...)                                         \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#include "MachOLiftedLeaf.def"

using macho_arm64e_abi::PAuthEncoding;

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

// Match the lifter's shift-and-mask expansion by bit provenance. A partial
// intermediate must stay intact until the complete permutation is examined.
void canonicalizeRegisterReversals(llvm::Function &F) {
  for (auto &Block : F) {
    for (auto &I : llvm::reverse(Block)) {
      if (I.use_empty() || I.getOpcode() != llvm::Instruction::Or)
        continue;
      llvm::SmallVector<llvm::Instruction *, 4> Inserted;
      if (llvm::recognizeBSwapOrBitReverseIdiom(&I, true, true, Inserted))
        I.replaceAllUsesWith(Inserted.back());
    }
  }
}

// The portable divide protects LLVM's partial operation twice when both the
// lifter and the emitter guard it. Match the zero result and every substitute
// divisor before selecting the target intrinsic with architectural semantics.
void canonicalizeDarwinDivision(llvm::SelectInst &Result) {
  using namespace llvm::PatternMatch;
  llvm::Value *Numerator, *Denominator, *Guarded;
  if (!match(Result.getCondition(),
             m_c_SpecificICmp(llvm::ICmpInst::ICMP_EQ, m_Value(Denominator),
                              m_ZeroInt())) ||
      !match(Result.getTrueValue(), m_ZeroInt()))
    return;
  auto *Div = llvm::dyn_cast<llvm::BinaryOperator>(Result.getFalseValue());
  if (!Div || !match(Div, m_IDiv(m_Value(Numerator), m_Value(Guarded))) ||
      Div->isExact() ||
      (!Div->getType()->isIntegerTy(sizeof(uint32_t) * CHAR_BIT) &&
       !Div->getType()->isIntegerTy(sizeof(uint64_t) * CHAR_BIT)))
    return;
  const bool Signed = Div->getOpcode() == llvm::Instruction::SDiv;
  bool GuardsOverflow = false;
  while (Guarded != Denominator) {
    // Replacing a zero divisor by one can also be canonicalized to umax(d, 1).
    llvm::Value *Base, *Guard;
    if (match(Guarded, m_UMax(m_Value(Base), m_One())) ||
        match(Guarded, m_UMax(m_One(), m_Value(Base)))) {
      Guarded = Base;
      continue;
    }
    if (!match(Guarded, m_Select(m_Value(Guard), m_One(), m_Value(Base))))
      return;
    const auto Zero = m_c_SpecificICmp(llvm::ICmpInst::ICMP_EQ,
                                       m_Specific(Base), m_ZeroInt());
    const auto Minimum = m_c_SpecificICmp(llvm::ICmpInst::ICMP_EQ,
                                          m_Specific(Numerator), m_SignMask());
    const auto MinusOne = m_c_SpecificICmp(llvm::ICmpInst::ICMP_EQ,
                                           m_Specific(Base), m_AllOnes());
    const auto Overflow = m_c_And(Minimum, MinusOne);
    if (!match(Guard, Zero)) {
      if (!Signed ||
          !(match(Guard, Overflow) || match(Guard, m_c_Or(Zero, Overflow))))
        return;
      GuardsOverflow = true;
    }
    Guarded = Base;
  }
  if (Signed && !GuardsOverflow)
    return;
  auto ID =
      Signed ? llvm::Intrinsic::aarch64_sdiv : llvm::Intrinsic::aarch64_udiv;
  auto *Native = llvm::IRBuilder<>(&Result).CreateIntrinsic(
      ID, {Div->getType()}, {Numerator, Denominator});
  Result.replaceAllUsesWith(Native);
}

void canonicalizeLeafOperations(llvm::Function &F,
                                llvm::FunctionAnalysisManager &Analyses) {
  const bool HasBranches = llvm::any_of(F, [](const auto &BB) {
    using namespace llvm::PatternMatch;
    return match(BB.getTerminator(), m_Br(m_Value(), m_Value(), m_Value()));
  });
  const bool HasSelects =
      llvm::any_of(llvm::instructions(F), [](const auto &I) {
        return llvm::isa<llvm::SelectInst>(I);
      });
  if (HasBranches || HasSelects) {
    llvm::FunctionPassManager Combine;
    Combine.addPass(llvm::InstCombinePass());
    if (HasBranches) {
      Combine.addPass(llvm::SimplifyCFGPass());
      // Darwin scheduling avoids extending the address register's live range
      // across a conditional store. Tuning does not enable any ISA features.
      if (!F.hasFnAttribute(kTuneCPUAttribute))
        F.addFnAttr(kTuneCPUAttribute, kClassicDarwinTuneCPU);
    }
    Combine.run(F, Analyses);
  }
  canonicalizeRegisterReversals(F);
  for (auto &I : llvm::instructions(F)) {
    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      if (!Select->use_empty())
        canonicalizeDarwinDivision(*Select);
      continue;
    }
    // Keep the compare for commutative min/max in Darwin argument order.
    auto *MinMax = llvm::dyn_cast<llvm::MinMaxIntrinsic>(&I);
    if (!MinMax)
      continue;
    auto *Left = llvm::dyn_cast<llvm::Argument>(MinMax->getArgOperand(0));
    auto *Right = llvm::dyn_cast<llvm::Argument>(MinMax->getArgOperand(1));
    if (Left && Right && Left->getArgNo() > Right->getArgNo()) {
      MinMax->setArgOperand(0, Right);
      MinMax->setArgOperand(1, Left);
    }
  }
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
  default:
    break;
  }
  // A literal LDR has no base register. Accept only the scalar address forms
  // whose decoded base, width and extension the unchanged word reproduces.
  const auto Memory =
      aarch64AddressMemoryInstruction(readLE<uint32_t>(I.Raw->bytes));
  return Memory && !Memory->hasRegisterIndex() &&
         Detail.op_count == kMemoryOperandCount &&
         Detail.operands[0].type == AARCH64_OP_REG &&
         leafRegister(Detail.operands[0].reg, !Memory->isLoad()) &&
         Detail.operands[1].type == AARCH64_OP_MEM &&
         (Detail.operands[1].mem.base == AARCH64_REG_SP ||
          leafRegister(Detail.operands[1].mem.base, true));
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
      readLE<uint32_t>(Source.end() - Word) != macho_arm64e_abi::kReturnToLR ||
      readLE<uint32_t>(Emitted.end() - Word) != macho_arm64e_abi::kReturnToLR)
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
    if (aarch64AddressMemoryInstruction(readLE<uint32_t>(I.Raw->bytes)) ||
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
llvm::Error bindLeafErrorResults(llvm::Module &Module,
                                 const llvm::DenseSet<va_t> &NativeEntries) {
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
      // An already verified native body passes X21 physically. Rebind its
      // symbolic callee, but do not rewrite the caller's LLVM prototype.
      if (Call && Call->isInlineAsm() && Call->isArgOperand(&Use) &&
          !Call->hasOperandBundles()) {
        auto Source = rewrite_source::getOriginalVA(*Call->getFunction());
        if (!Source)
          return Source.takeError();
        if (*Source && NativeEntries.contains(**Source))
          continue;
      }
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
          static_cast<uint32_t>(PAuthEncoding::PACIBSP))
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

// An inserted word belongs to the original ADR. A boundary at that ADR
// still names its first word, while every later boundary moves.
struct AddressExpansion {
  llvm::SmallVector<uint64_t, 2> Sites;

  uint64_t operator()(uint64_t Offset) const {
    const auto Word = llvm::alignDown(Offset, aarch64::kInsnSize);
    return Offset + aarch64::kInsnSize *
                        (llvm::lower_bound(Sites, Word) - Sites.begin());
  }
};

// Compare effective rules at their code locations. A removed signing opcode
// can leave two DW_CFA_advance_loc instructions for one unchanged interval.
bool sameSpillRules(llvm::ArrayRef<CFIInstruction> Old,
                    llvm::ArrayRef<CFIInstruction> New, uint64_t CodeSize = 0,
                    uint64_t Alignment = 1, const AddressExpansion &Growth = {},
                    bool AllowPCSigning = false) {
  // The decoder advances monotonically. Check even a final advance with no
  // subsequent rule, which would otherwise disappear from the comparison.
  const auto InRange = [Alignment](auto Program, uint64_t Size) {
    if (Program.empty())
      return true;
    const auto &Last = Program.back();
    return Last.CodeOffset <= Size &&
           (Last.Kind != CFIOpKind::AdvanceLoc ||
            uint64_t(Last.Offset) <= (Size - Last.CodeOffset) / Alignment);
  };
  if (!InRange(Old, CodeSize) || !InRange(New, Growth(CodeSize)))
    return false;
  const auto Effective = [](const CFIInstruction &I) {
    return I.Kind != CFIOpKind::Nop && I.Kind != CFIOpKind::AdvanceLoc;
  };
  auto Before = llvm::make_filter_range(Old, Effective);
  auto After = llvm::make_filter_range(New, Effective);
  return llvm::all_of(Before,
                      [AllowPCSigning](const auto &I) {
                        switch (I.Kind) {
                        case CFIOpKind::DefCFA:
                        case CFIOpKind::DefCFARegister:
                        case CFIOpKind::DefCFAOffset:
                        case CFIOpKind::Offset:
                        case CFIOpKind::Restore:
                        case CFIOpKind::SameValue:
                        case CFIOpKind::RememberState:
                        case CFIOpKind::RestoreState:
                        case CFIOpKind::GnuArgsSize:
                          return true;
                        case CFIOpKind::NegateRAStateWithPC:
                          return AllowPCSigning;
                        default:
                          return false;
                        }
                      }) &&
         llvm::equal(Before, After,
                     [&](const CFIInstruction &A, const CFIInstruction &B) {
                       if (Growth.Sites.empty())
                         return dwarf_eh::sameCFIInstruction(A, B);
                       auto Mapped = A;
                       Mapped.CodeOffset = Growth(A.CodeOffset);
                       return dwarf_eh::sameCFIInstruction(Mapped, B);
                     });
}

// The compact table points to a specific FDE. Compare the compiler's LR
// spill and its return rules at every original instruction boundary.
bool sameIndexedLinkFrame(const BinaryImage &Original,
                          const ExceptionFunction &Frame,
                          const dwarf_eh::ParseResult &Generated,
                          const DwarfFDE *NewFrame, va_t Start,
                          const NativeMachOCompactRange &Range, va_t Slide,
                          uint64_t RetainedBytes,
                          const AddressExpansion &Growth) {
  const auto CodeSize = Range.EndVA - Range.BeginVA;
  const bool Language = Frame.Itanium.has_value();
  if (!Frame.Dwarf || !NewFrame || Frame.Dwarf->InitialLocation != Start ||
      Frame.Dwarf->AddressRange < RetainedBytes ||
      Growth(Frame.Dwarf->AddressRange - RetainedBytes) != CodeSize ||
      Frame.Dwarf->ParseStatus != ExceptionParseStatus::Complete ||
      (Frame.Compact->NativeEncoding & macho_unwind::kDwarfSectionOffsetMask) !=
          Frame.Dwarf->SectionOffset ||
      Generated.ParseStatus != ExceptionParseStatus::Complete)
    return false;
  const auto *Old =
      Original.ExceptionMetadata.findCIE(Frame.Dwarf->CIESectionOffset);
  if (!Old ||
      (!Language && Old->Augmentation != kCompilerFrameAugmentation &&
       Old->Augmentation != kBKeyFrameAugmentation) ||
      Old->ParseStatus != ExceptionParseStatus::Complete ||
      (!Language && NewFrame->LSDAVA) || NewFrame->AddressRange != CodeSize ||
      NewFrame->ParseStatus != ExceptionParseStatus::Complete)
    return false;
  const auto New = llvm::lower_bound(
      Generated.CIEs, NewFrame->CIESectionOffset,
      [](const DwarfCIE &C, uint64_t Off) { return C.SectionOffset < Off; });
  const auto ABI = [](const DwarfCIE &C) {
    return std::tie(C.Version, C.Augmentation, C.CodeAlignmentFactor,
                    C.DataAlignmentFactor, C.ReturnAddressRegister,
                    C.AddressSize, C.SegmentSelectorSize, C.FDEPointerEncoding,
                    C.UsesArm64eReturnAddressABI, C.PersonalityEncoding,
                    C.LSDAPointerEncoding, C.IsSignalFrame, C.HasPointerAuth,
                    C.UsesNonHintPAuthLR, C.HasMTETaggedFrame, C.ParseStatus);
  };
  return New != Generated.CIEs.end() &&
         New->SectionOffset == NewFrame->CIESectionOffset &&
         ABI(*Old) == ABI(*New) &&
         New->PersonalitySlotVA ==
             (Language ? Old->PersonalitySlotVA + Slide : 0) &&
         Range.LSDAVA.value_or(0) == NewFrame->LSDAVA &&
         Range.PersonalitySlotVA.value_or(0) == New->PersonalitySlotVA &&
         sameSpillRules(Old->InitialInstructions, New->InitialInstructions) &&
         sameSpillRules(Frame.Dwarf->Instructions, NewFrame->Instructions,
                        Frame.Dwarf->AddressRange - RetainedBytes,
                        Old->CodeAlignmentFactor, Growth);
}

bool sameLanguageTable(const ExceptionFunction &Before,
                       const BinaryImage &Installed,
                       const NativeMachOCompactRange &Range, va_t Slide,
                       const AddressExpansion &Growth, bool Rebuilt) {
  const auto *After = Installed.ExceptionMetadata.findFunction(Range.BeginVA);
  if (!After || After->ParseStatus != ExceptionParseStatus::Complete ||
      !After->Itanium || After->CodeRange.Begin != Range.BeginVA ||
      After->CodeRange.End != Range.EndVA ||
      After->Personality != Before.Personality)
    return false;
  const auto &Old = *Before.Itanium;
  const auto &New = *After->Itanium;
  if (Range.LSDAVA.value_or(0) != New.LSDAVA ||
      Old.IsCallSiteAddressForm != New.IsCallSiteAddressForm ||
      Old.TypeTableEntryKind != New.TypeTableEntryKind ||
      Old.CallSites.size() != New.CallSites.size() ||
      Old.Actions.size() != New.Actions.size() ||
      Old.TypeTable.size() != New.TypeTable.size() ||
      Old.ExceptionSpecs.size() != New.ExceptionSpecs.size())
    return false;
  if (!Rebuilt && New.LSDAVA != Old.LSDAVA + Slide)
    return false;
  // Explicit source bases may be canonicalized. Compare the normalized
  // guard and handler coordinates below, including entry-point handlers.

  for (size_t I = 0; I < Old.CallSites.size(); ++I) {
    const auto &A = Old.CallSites[I];
    const auto &B = New.CallSites[I];
    if (Growth(A.GuardedRange.Begin - Before.CodeRange.Begin) !=
            B.GuardedRange.Begin - Range.BeginVA ||
        Growth(A.GuardedRange.End - Before.CodeRange.Begin) !=
            B.GuardedRange.End - Range.BeginVA ||
        A.FirstActionOffset != B.FirstActionOffset ||
        (A.LandingPadVA ? (!B.LandingPadVA ||
                           Growth(A.LandingPadVA - Before.CodeRange.Begin) !=
                               B.LandingPadVA - Range.BeginVA)
                        : B.LandingPadVA != 0))
      return false;
  }
  for (size_t I = 0; I < Old.Actions.size(); ++I) {
    const auto &A = Old.Actions[I];
    const auto &B = New.Actions[I];
    if (std::tie(A.TableOffset, A.TypeFilter, A.NextActionOffset) !=
        std::tie(B.TableOffset, B.TypeFilter, B.NextActionOffset))
      return false;
  }
  for (size_t I = 0; I < Old.TypeTable.size(); ++I) {
    const auto &A = Old.TypeTable[I];
    const auto &B = New.TypeTable[I];
    if (A.Index != B.Index || A.IsCatchAll != B.IsCatchAll ||
        (A.TypeInfoSlotVA
             ? B.TypeInfoSlotVA != A.TypeInfoSlotVA + Slide
             : B.TypeInfoSlotVA ||
                   B.TypeInfoVA != (A.TypeInfoVA ? A.TypeInfoVA + Slide : 0)))
      return false;
  }
  for (size_t I = 0; I < Old.ExceptionSpecs.size(); ++I) {
    const auto &A = Old.ExceptionSpecs[I];
    const auto &B = New.ExceptionSpecs[I];
    if (A.Index != B.Index || A.TypeIndices != B.TypeIndices)
      return false;
  }
  return true;
}

bool sameCompactFrameRows(const ExceptionFunction &Original,
                          const dwarf_eh::ParseResult &Generated,
                          const DwarfFDE *Frame,
                          const NativeMachOCompactRange &Range, va_t Slide,
                          llvm::ArrayRef<CFIInstruction> Rows,
                          llvm::StringRef Augmentation) {
  const bool Language = Original.Itanium.has_value();
  if (!Frame || Frame->InitialLocation != Range.BeginVA ||
      Frame->AddressRange != Range.EndVA - Range.BeginVA ||
      (Language ? Frame->LSDAVA != Range.LSDAVA.value_or(0) : Frame->LSDAVA) ||
      Frame->ParseStatus != ExceptionParseStatus::Complete ||
      Generated.ParseStatus != ExceptionParseStatus::Complete)
    return false;
  const auto CIE = llvm::find_if(Generated.CIEs, [&](const auto &C) {
    return C.SectionOffset == Frame->CIESectionOffset;
  });
  const CFIInstruction Initial{.Kind = CFIOpKind::DefCFA,
                               .Register = aarch64_unwind::kSP};
  return CIE != Generated.CIEs.end() &&
         CIE->ParseStatus == ExceptionParseStatus::Complete &&
         (Language ? CIE->PersonalityEncoding ==
                             (dweh::Indirect | dweh::PCRel | dweh::Sdata4) &&
                         CIE->PersonalitySlotVA ==
                             Original.Compact->PersonalitySlotVA + Slide &&
                         CIE->PersonalitySlotVA ==
                             Range.PersonalitySlotVA.value_or(0) &&
                         CIE->LSDAPointerEncoding == dweh::PCRel &&
                         !CIE->IsSignalFrame && !CIE->UsesNonHintPAuthLR &&
                         !CIE->HasMTETaggedFrame && CIE->HasPointerAuth
                   : CIE->Augmentation == Augmentation &&
                         !Range.PersonalitySlotVA && !Range.LSDAVA) &&
         CIE->UsesArm64eReturnAddressABI &&
         CIE->ReturnAddressRegister == aarch64_unwind::kLR &&
         sameSpillRules({Initial}, CIE->InitialInstructions) &&
         sameSpillRules(
             Rows, Frame->Instructions, Frame->AddressRange,
             CIE->CodeAlignmentFactor, {},
             Original.Compact->ReturnAddressSigningOffset.has_value());
}

// Discard just the fallback FDEs whose complete rules are represented by
// compact unwind. A mixed object still needs its original indexed functions,
// including their shared CIE, personality slots and LSDA pointers.
llvm::Error
retainIndexedFrames(CompiledSection &EH, va_t SectionVA,
                    const dwarf_eh::ParseResult &Frames,
                    const llvm::DenseSet<va_t> &Replaced,
                    llvm::MutableArrayRef<NativeMachOCompactRange> Ranges) {
  auto Records = decodeDwarfEHFrameRecords(EH.ExternalBytes, EH.VA, true);
  if (!Records)
    return Records.takeError();
  if (Records->size() != Frames.FDEs.size())
    return refuse(kIndexedFrameRelocation);
  if (Replaced.size() == Records->size()) {
    EH.ExternalBytes.clear();
    EH.Size = 0;
    return llvm::Error::success();
  }
  std::map<uint64_t, uint64_t> Kept;
  llvm::DenseMap<uint64_t, uint64_t> CIEs;
  for (const auto &F : Frames.FDEs) {
    if (Replaced.contains(F.SectionOffset))
      continue;
    Kept.try_emplace(F.CIESectionOffset);
    Kept.try_emplace(F.SectionOffset);
    CIEs.try_emplace(F.SectionOffset, F.CIESectionOffset);
  }
  const llvm::ArrayRef<uint8_t> Bytes(EH.ExternalBytes);
  std::vector<uint8_t> Packed;
  for (auto &[Old, New] : Kept) {
    New = Packed.size();
    const auto *Header = Bytes.data() + Old;
    const auto Length = readLE<uint32_t>(Header);
    const auto Size =
        Length == dweh::kDwarf64LengthEscape
            ? dweh::kRecordLengthSize + dweh::kDwarf64LengthSize +
                  readLE<uint64_t>(Header + dweh::kRecordLengthSize)
            : dweh::kRecordLengthSize + Length;
    llvm::append_range(Packed, Bytes.slice(Old, Size));
  }
  // A copied CFI program keeps its code-relative row locations. Only the
  // CIE back-reference and section-relative pointer fields move.
  const auto MovePointer = [&](const DwarfEHFramePointer &P, uint64_t Shift,
                               bool Nullable) {
    if (!Shift || (Nullable && !P.Value))
      return true;
    if (dweh::getApplication(P.Encoding) != dweh::PCRel)
      return false;
    const auto Offset = P.Offset - Shift;
    const unsigned Wide = sizeof(va_t) * CHAR_BIT + 1;
    const auto Delta =
        llvm::APInt(Wide, P.Value) - llvm::APInt(Wide, EH.VA + Offset);
    if (!Delta.isSignedIntN(P.Width * CHAR_BIT) || (Nullable && Delta.isZero()))
      return false;
    const auto Value = Delta.getLoBits(P.Width * CHAR_BIT).getZExtValue();
    if (P.Width == sizeof(uint32_t))
      writeLE<uint32_t>(Packed.data() + Offset, Value);
    else if (P.Width == sizeof(uint64_t))
      writeLE<uint64_t>(Packed.data() + Offset, Value);
    else
      return false;
    return true;
  };
  for (const auto &F : *Records) {
    const auto Old = F.RecordVA - EH.VA;
    if (Replaced.contains(Old))
      continue;
    const auto Owner = CIEs.find(Old);
    if (Owner == CIEs.end())
      return refuse(kIndexedFrameRelocation);
    const auto CIE = Owner->second;
    const auto Shift = Old - Kept.at(Old);
    const auto Field = F.InitialLocationOffset - Shift;
    const bool Extended =
        readLE<uint32_t>(Bytes.data() + Old) == dweh::kDwarf64LengthEscape;
    const auto ID = Field - (Extended ? sizeof(uint64_t) : sizeof(uint32_t));
    if (Extended)
      writeLE<uint64_t>(Packed.data() + ID, ID - Kept.at(CIE));
    else
      writeLE<uint32_t>(Packed.data() + ID, ID - Kept.at(CIE));
    if (!MovePointer({F.InitialLocationOffset, F.BeginVA,
                      F.InitialLocationEncoding, F.InitialLocationWidth},
                     Shift, false) ||
        (F.Personality &&
         !MovePointer(*F.Personality, CIE - Kept.at(CIE), true)) ||
        (F.LSDA && !MovePointer(*F.LSDA, Shift, true)))
      return refuse(kIndexedFrameRelocation);
  }
  const auto Base = EH.VA - SectionVA;
  for (auto &Range : Ranges) {
    if ((Range.Encoding & macho_unwind::kModeMask) !=
        macho_unwind::kARM64ModeDwarf)
      continue;
    const auto Mask = macho_unwind::kDwarfSectionOffsetMask;
    const auto It = Kept.find((Range.Encoding & Mask) - Base);
    if (It == Kept.end())
      return refuse(kIndexedFrameRelocation);
    Range.Encoding = (Range.Encoding & ~Mask) | uint32_t(Base + It->second);
  }
  EH.ExternalBytes = std::move(Packed);
  EH.Size = EH.ExternalBytes.size();
  return llvm::Error::success();
}

// A classic PACIBSP frame may be indexed by DWARF even when the original
// used a compact recipe. Match its rules against the actual frame record.
bool sameSignedCompactFrame(const ExceptionFunction &Original,
                            const dwarf_eh::ParseResult &Generated,
                            const DwarfFDE *Frame,
                            const NativeMachOCompactRange &Range,
                            llvm::ArrayRef<uint8_t> Code) {
  constexpr auto Word = aarch64::kInsnSize;
  if (Original.Dwarf || Original.Itanium || Code.size() < 2 * Word ||
      readLE<uint32_t>(Code.data()) !=
          static_cast<uint32_t>(PAuthEncoding::PACIBSP) ||
      readLE<uint32_t>(Code.end() - Word) !=
          static_cast<uint32_t>(PAuthEncoding::RETAB))
    return false;
  llvm::SmallVector<CFIInstruction> Rows;
  if (Original.Compact->NativeEncoding == macho_unwind::kARM64ModeFrame) {
    auto Body = signedFrameBody(Code, kPushFrame, kPopFrame);
    if (!Body)
      Body = signedFrameBody(Code, kAllocateFrame, kDeallocateFrame);
    if (!Body)
      return false;
    constexpr int64_t Slot = aarch64_unwind::kRegisterBytes;
    Rows = {{.Kind = CFIOpKind::DefCFA,
             .CodeOffset = Body->Begin,
             .Register = aarch64_unwind::kFP,
             .Offset = 2 * Slot},
            {.Kind = CFIOpKind::Offset,
             .CodeOffset = Body->Begin,
             .Register = aarch64_unwind::kLR,
             .Offset = -Slot},
            {.Kind = CFIOpKind::Offset,
             .CodeOffset = Body->Begin,
             .Register = aarch64_unwind::kFP,
             .Offset = -2 * Slot}};
  } else if (Original.Compact->NativeEncoding !=
             macho_unwind::kARM64ModeFrameless)
    return false;
  return sameCompactFrameRows(Original, Generated, Frame, Range, 0, Rows,
                              kCompilerFrameAugmentation);
}

// Compare on-disk bytes as well as the loaded view: dyld can normalize pointer
// slots in a segment buffer. An opaque suffix keeps its original permissions.
std::pair<llvm::ArrayRef<uint8_t>, SegmentFlags>
retainedFileMapping(const BinaryImage &Image, va_t Start, uint64_t Size) {
  const auto *Segment = Image.getSegmentFor(Start);
  if (!Segment || !Segment->isExecutable() || Segment->isWritable() ||
      !rangeInBounds(Start - Segment->VA, Size, Segment->FileSz))
    return {};
  const uint64_t Offset = Image.vaToFileOffset(Start);
  if (!rangeInBounds(Offset, Size, Image.Raw.size()))
    return {};
  return {llvm::ArrayRef(Image.Raw).slice(Offset, Size), Segment->Flags};
}

llvm::Error
verifyLeaves(const BinaryImage &Original, const BinaryImage &Layout,
             llvm::ArrayRef<SourcePatchMapping> Mappings,
             const CompiledImage &Generated, va_t Slide,
             llvm::ArrayRef<NativeMachOCompactRange> Ranges,
             const llvm::DenseSet<va_t> &RegisterEntries,
             const llvm::DenseMap<va_t, ExceptionAddressRange> &RetainedTails,
             llvm::ArrayRef<MachODataInCodeRange> SourceData,
             llvm::ArrayRef<MachOLiftedSourceAddress> Addresses,
             const CompiledSection *AddressPool) {
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  const CompiledSection *Code = nullptr;
  dwarf_eh::ParseResult LinkedFrames;
  for (const auto &Section : Generated.Sections) {
    if (Section.Kind != Kind::Code) {
      if (&Section == AddressPool && Section.Kind == Kind::WritableData &&
          Section.ReadOnlyAfterFixups &&
          Layout.readOnlyAfterRelocations(Section.VA, Section.Size))
        continue;
      if (Section.Name == section_names::macho::EhFrame &&
          !Section.ExternalBytes.empty())
        LinkedFrames =
            dwarf_eh::parseEHFrame(Layout,
                                   {Section.ExternalBytes.data(),
                                    Section.ExternalBytes.size(), Section.VA},
                                   {});
      if (Section.Name != section_names::macho::CompactUnwind &&
          Section.Name != section_names::macho::EhFrame &&
          Section.Name != section_names::macho::GccExceptTab)
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
    return llvm::createStringError(llvm::errc::not_supported,
                                   llvm::formatv(kGeneratedCoverage,
                                                 Ranges.size(), Mappings.size(),
                                                 Code ? Code->Size : 0)
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

  for (const auto &R : SourceData) {
    const auto Size = R.End - R.Begin;
    const auto Installed =
        machODataInCodeRanges(Layout.MachODataInCode, R.Begin + Slide, Size);
    if (Installed.size() != 1 || Installed.front().Begin != R.Begin + Slide ||
        Installed.front().End != R.End + Slide ||
        Installed.front().Kind != R.Kind)
      return refuse(kEmbeddedIdentity);
    // Relocated source records can have explicit fixup owners. Each recovered
    // read was proved immutable during register lowering.
    if (machODataInCodeRanges(Original.MachODataInCode, R.Begin, Size)
            .empty()) {
      const auto *Before = Original.readVA(R.Begin, Size);
      const auto *After = Layout.readVA(R.Begin + Slide, Size);
      if (!Before || !After || std::memcmp(Before, After, Size))
        return refuse(kEmbeddedIdentity);
    }
  }
  Decoder Decode;
  if (!Decode.init(Original))
    return refuse(kLeafCode);
  for (size_t I = 0; I < Mappings.size(); ++I) {
    const auto &Mapping = Mappings[I];
    const va_t Start = Mapping.OriginalVA;
    auto SourceBody = machOLiftedSource(
        Original, Start,
        I + 1 == Mappings.size() ? InvalidVA : Mappings[I + 1].OriginalVA);
    if (!SourceBody)
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kSourceExtent, Mapping.Symbol, Start).str());
    if (auto It = RetainedTails.find(Start); It != RetainedTails.end()) {
      const auto Tail = It->second;
      if (Tail.End - Start != SourceBody->Bytes.size())
        return refuse(kRetainedAddressTail);
      const auto *After = Layout.readVA(Tail.Begin + Slide, Tail.size());
      const auto Before = SourceBody->Bytes.drop_front(Tail.Begin - Start);
      const auto File = retainedFileMapping(Original, Tail.Begin, Tail.size());
      if (!After || File.first.empty() ||
          File !=
              retainedFileMapping(Layout, Tail.Begin + Slide, Tail.size()) ||
          std::memcmp(Before.data(), After, Before.size()) ||
          !machODataInCodeRanges(Layout.MachODataInCode, Tail.Begin + Slide,
                                 Tail.size())
               .empty())
        return refuse(kRetainedAddressTail);
      SourceBody->Bytes = SourceBody->Bytes.take_front(Tail.Begin - Start);
    }
    SourceBody->Embedded =
        machODataInCodeRanges(SourceData, Start, SourceBody->Bytes.size());
    const auto Source = SourceBody->Bytes;
    const va_t End = Start + Source.size();
    AddressExpansion InstalledOffset;
    bool RebuiltLSDA = false;
    for (const auto &Site : Addresses) {
      if (Site.Instruction >= End)
        break;
      RebuiltLSDA |= Site.Form != MachOAddressForm::PagePair ||
                     Site.insertsSigningModifier();
      if (Site.Form == MachOAddressForm::Expanded)
        InstalledOffset.Sites.push_back(Site.Instruction - Start);
      if (Site.insertsSigningModifier())
        InstalledOffset.Sites.push_back(Site.Authentication->Instruction -
                                        Start);
    }
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
    const auto Native = Range.mapped()->Encoding;
    const auto Emitted = llvm::ArrayRef(Generated.Bytes)
                             .slice(Code->Offset + (Owner->second - Code->VA),
                                    Range.mapped()->EndVA - Owner->second);
    const auto PublishedData = machODataInCodeRanges(
        Layout.MachODataInCode, Owner->second, Emitted.size());
    if (PublishedData.size() != SourceBody->Embedded.size())
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kEmbeddedCount, Mapping.Symbol,
                        SourceBody->Embedded.size(), PublishedData.size())
              .str());
    for (auto [Old, New] : llvm::zip_equal(SourceBody->Embedded, PublishedData))
      if (New.Begin - Owner->second != InstalledOffset(Old.Begin - Start) ||
          New.End - Owner->second != InstalledOffset(Old.End - Start) ||
          New.Kind != Old.Kind)
        return refuse(kEmbeddedIdentity);
    if (!SourceBody->Embedded.empty() && !RegisterEntries.contains(Start))
      return refuse(kEmbeddedIdentity);
    if (SourceBody->ForwardTarget) {
      // The entry branch has no register, stack or LR side effects. Its old
      // trailing body and unwind recipe do not describe this forwarding path.
      if (!RegisterEntries.contains(Start) ||
          Emitted.size() != aarch64::kInsnSize ||
          Native != macho_unwind::kARM64ModeFrameless ||
          Range.mapped()->LSDAVA || Range.mapped()->PersonalitySlotVA ||
          FDEs.count(Owner->second) ||
          !matchesDirectTransfer(Source, Start, Emitted, Owner->second,
                                 Mappings, Generated, Original, Slide,
                                 {.Jumps = true}))
        return refuse(kDirectTarget);
      continue;
    }
    if (!Frame || !Frame->Compact ||
        (!Frame->describesUnwindOnly() &&
         !(RegisterEntries.contains(Start) &&
           canPublishMachOLanguageFrame(Original, *Frame))) ||
        Frame->CodeRange.End < End)
      return refuse(kLeafFrame);
    if (Frame->Itanium &&
        !sameLanguageTable(
            *Frame, Layout, *Range.mapped(), Slide, InstalledOffset,
            RebuiltLSDA ||
                !canBindMachOLanguageTable(Original, *Frame->Itanium)))
      return refuse(kLeafFrame);
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
    if (InstalledOffset(Source.size()) != Emitted.size())
      return llvm::createStringError(llvm::errc::not_supported,
                                     llvm::formatv(kBodyExtent, Mapping.Symbol,
                                                   Source.size(),
                                                   Emitted.size())
                                         .str());
    const auto DwarfFields = macho_unwind::kDwarfSectionOffsetMask |
                             macho_unwind::kHasLSDA |
                             macho_unwind::kPersonalityMask;
    const bool Indexed =
        (Native & ~DwarfFields) == macho_unwind::kARM64ModeDwarf;
    const auto SourceEncoding = Frame->Compact->NativeEncoding;
    const bool FramelessSpills =
        RegisterEntries.contains(Start) &&
        Frame->Compact->Kind == CompactUnwindKind::FramelessImmediate &&
        !Frame->Compact->SavedRegisterSlots.empty();
    if (FramelessSpills) {
      if (!Indexed || Frame->Dwarf ||
          !sameCompactFrameRows(*Frame, LinkedFrames,
                                FDEs.lookup(Owner->second), *Range.mapped(),
                                Slide, machOFramelessSpillRows(*Frame->Compact),
                                kBKeyFrameAugmentation))
        return refuse(kLeafFrame);
    } else if (Indexed) {
      const auto *FDE = FDEs.lookup(Owner->second);
      if ((SourceEncoding & ~DwarfFields) == macho_unwind::kARM64ModeDwarf) {
        if (!sameIndexedLinkFrame(
                Original, *Frame, LinkedFrames, FDE, Start, *Range.mapped(),
                Slide, RetainedTails.lookup(Start).size(), InstalledOffset))
          return refuse(kLeafFrame);
      } else if (!sameSignedCompactFrame(*Frame, LinkedFrames, FDE,
                                         *Range.mapped(), Emitted))
        return refuse(kLeafFrame);
    } else {
      const bool Language = Frame->Itanium.has_value();
      // The object has no final personality-array index yet. Compare its
      // linked slot identity instead, together with the retained LSDA.
      if ((SourceEncoding & ~macho_unwind::kPersonalityMask) != Native ||
          Range.mapped()->PersonalitySlotVA.value_or(0) !=
              (Language ? Frame->Compact->PersonalitySlotVA + Slide : 0) ||
          (!Language && Range.mapped()->LSDAVA))
        return refuse(kLeafFrame);
    }

    if (RegisterEntries.contains(Start)) {
      // Verify every edge against the layout that includes the widened sites.
      // A call between functions must resolve to the installed entry.
      if (!Indexed && (FDEs.count(Owner->second) || Frame->Dwarf))
        return refuse(kLeafFrame);
      const MachOLiftedSourceAddress *SigningConsumer = nullptr;
      for (size_t Offset = 0; Offset < Source.size();
           Offset += aarch64::kInsnSize) {
        auto Before = Source.drop_front(Offset);
        auto After = Emitted.drop_front(InstalledOffset(Offset));
        if (!machODataInCodeRanges(SourceBody->Embedded, Start + Offset,
                                   aarch64::kInsnSize)
                 .empty()) {
          if (Before.take_front(aarch64::kInsnSize) !=
              After.take_front(aarch64::kInsnSize))
            return refuse(kLeafCode);
          continue;
        }
        if (SigningConsumer &&
            SigningConsumer->Authentication->Instruction == Start + Offset) {
          // Decode both instructions independently of the lowering plan. The
          // opcode preserves the IA/IB key and AUT/return behavior. Only the
          // second modifier changes representation.
          DecodedInsn I{};
          constexpr auto Size = aarch64::kInsnSize;
          if (Decode.decodeOne(Before.data(), Size, Start + Offset, I) !=
                  int(Size) ||
              !I.Raw || !I.Raw->detail ||
              I.Raw->detail->aarch64.op_count != 1 ||
              I.Raw->detail->aarch64.operands[0].type != AARCH64_OP_REG)
            return refuse(kSourceAddressTarget);
          const auto Id = I.Id;
          const auto Register =
              SigningConsumer->Authentication->ModifierRegister;
          const auto Prefix = Register ? Size : 0;
          if (After.size() < Prefix + Size)
            return refuse(kSourceAddressTarget);
          const va_t PC = Owner->second + InstalledOffset(Offset) + Prefix;
          if (Decode.decodeOne(After.data() + Prefix, Size, PC, I) !=
                  int(Size) ||
              I.Id != Id || !I.Raw || !I.Raw->detail ||
              I.Raw->detail->aarch64.op_count != 1)
            return refuse(kSourceAddressTarget);
          const va_t Sign =
              Owner->second + InstalledOffset(SigningConsumer->Target - Start);
          const auto &Modifier = I.Raw->detail->aarch64.operands[0];
          if (Register) {
            const uint32_t Setup = readLE<uint32_t>(After.data());
            if (Modifier.type != AARCH64_OP_REG ||
                unsigned(Modifier.reg - AARCH64_REG_X0) != *Register ||
                aarch64::encodePCRelativeAddress(
                    aarch64::AddressInstruction::ADR, *Register, PC - Size,
                    Sign) != Setup)
              return refuse(kSourceAddressTarget);
          } else {
            const auto Distance =
                ((readLE<uint32_t>(After.data()) >> kAArch64PAuthLRImmShift) &
                 kAArch64PAuthLRImmMask) *
                kAArch64PAuthLRScale;
            if (Modifier.type != AARCH64_OP_IMM || Sign > PC ||
                PC - Sign != Distance)
              return refuse(kSourceAddressTarget);
          }
          SigningConsumer = nullptr;
          continue;
        }
        if (!Addresses.empty() &&
            Addresses.front().Instruction == Start + Offset) {
          const auto &Fixup = Addresses.front();
          Addresses = Addresses.drop_front();
          const auto Word = readLE<uint32_t>(After.data());
          const auto PC = Owner->second + InstalledOffset(Offset);
          if (Fixup.Authentication)
            SigningConsumer = &Fixup;
          if (Fixup.Form != MachOAddressForm::Single) {
            const auto New = decodeMachOPageAddress(Decode, After, PC);
            if (!New || New->Target != Fixup.Target + Slide ||
                (Word & aarch64::kRdMask) !=
                    (readLE<uint32_t>(Before.data()) & aarch64::kRdMask))
              return refuse(kSourceAddressTarget);
            if (Fixup.Form == MachOAddressForm::PagePair) {
              const auto Old =
                  decodeMachOPageAddress(Decode, Before, Start + Offset);
              if (!Old || Old->Target != Fixup.Target ||
                  New->PageRegister != Old->PageRegister ||
                  New->ResultRegister != Old->ResultRegister)
                return refuse(kDataPagePair);
              Offset += aarch64::kInsnSize;
            } else if (New->PageRegister != New->ResultRegister) {
              return refuse(kSourceAddressTarget);
            }
            continue;
          }
          auto Target = machORebasedLiteralAddress(Layout, Word, PC);
          if (!Target) {
            DecodedInsn ADR{};
            if (Decode.decodeOne(After.data(), aarch64::kInsnSize, PC, ADR) !=
                    aarch64::kInsnSize ||
                ADR.Id != AARCH64_INS_ADR || !ADR.Raw || !ADR.Raw->detail)
              return refuse(kSourceAddressTarget);
            Target = ADR.Raw->detail->aarch64.operands[1].imm;
          }
          if ((readLE<uint32_t>(Before.data()) & aarch64::kRdMask) !=
                  (Word & aarch64::kRdMask) ||
              *Target != Fixup.Target + Slide)
            return refuse(kSourceAddressTarget);
          continue;
        }
        if ((readLE<uint32_t>(Before.data()) & aarch64::kADRP_Mask) ==
            aarch64::kADRP_Op) {
          const auto Old =
              decodeMachOPageAddress(Decode, Before, Start + Offset);
          const auto New = decodeMachOPageAddress(
              Decode, After, Owner->second + InstalledOffset(Offset));
          if (!Old || !New || New->Target != Old->Target + Slide ||
              New->PageRegister != Old->PageRegister ||
              New->ResultRegister != Old->ResultRegister)
            return refuse(kDataPagePair);
          Offset += aarch64::kInsnSize;
          continue;
        }
        const auto Transfer = decodeDirectBranch(
            Original.Arch, InstructionMode::Default, Before.data(),
            Before.size(), Start + Offset, {.Jumps = true, .Calls = true});
        const auto Target = Transfer ? Transfer->target() : std::nullopt;
        if (Target && (*Target < Start || *Target >= End)) {
          if (!matchesDirectTransfer(Before, Start + Offset, After,
                                     Owner->second + InstalledOffset(Offset),
                                     Mappings, Generated, Original, Slide,
                                     {.Jumps = true, .Calls = true}))
            return refuse(kDirectTarget);
        } else if (!InstalledOffset.Sites.empty() &&
                   (Target || branch::a64ConditionalBranchDisplacement(
                                  readLE<uint32_t>(Before.data())))) {
          const auto Word = readLE<uint32_t>(Before.data());
          const va_t Destination =
              Target ? *Target
                     : Start + Offset +
                           *branch::a64ConditionalBranchDisplacement(Word);
          if (Destination < Start || Destination >= End)
            return refuse(kDirectTarget);
          const auto Delta = int64_t(InstalledOffset(Destination - Start)) -
                             int64_t(InstalledOffset(Offset));
          std::optional<uint32_t> Expected;
          if (!Target)
            Expected =
                branch::withA64ConditionalBranchDisplacement(Word, Delta);
          else if (branch::isA64BranchDisplacement(Delta))
            Expected = branch::withA64BranchDisplacement(Word, Delta);
          if (Expected != readLE<uint32_t>(After.data()))
            return refuse(kDirectTarget);
        } else if (!equivalentLeafWord(readLE<uint32_t>(Before.data()),
                                       readLE<uint32_t>(After.data()))) {
          return refuse(kLeafCode);
        }
      }
      continue;
    }

    const bool Framed = SourceEncoding == macho_unwind::kARM64ModeFrame;
    std::optional<FrameBody> Body;
    if (Framed) {
      Body = signedFrameBody(Source, kPushFrame, kPopFrame);
      if (!Body)
        Body = signedFrameBody(Source, kAllocateFrame, kDeallocateFrame);
      if (!Body)
        return refuse(kLeafFrame);
    } else if ((SourceEncoding & ~DwarfFields) ==
               macho_unwind::kARM64ModeDwarf) {
      Body = signedFrameBody(Source, kPushLink, kPopLink);
      if (!Body)
        return refuse(kLeafFrame);
    } else if (SourceEncoding != macho_unwind::kARM64ModeFrameless) {
      return refuse(kLeafFrame);
    }
    bool Signed = false;
    size_t MatchedEnd = 0;
    llvm::SmallVector<va_t, 4> BranchTargets, SwappedSeconds;
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
        SwappedSeconds.push_back(Start + Offset + aarch64::kInsnSize);
      }
      const auto &D = Insn.Raw->detail->aarch64;
      switch (Insn.Id) {
#define MACHO_LEAF_LOCAL_JUMP(Name) case Name:
#include "MachOLiftedLeaf.def"
        // Both bodies have the same size and instruction offsets. An equal
        // branch word is position-independent only inside this function.
        if (Last || !D.op_count ||
            D.operands[D.op_count - 1].type != AARCH64_OP_IMM)
          return refuse(kDirectTarget);
        if (const va_t Target = D.operands[D.op_count - 1].imm;
            Target >= Start && Target < End)
          BranchTargets.push_back(Target);
        else
          return refuse(kDirectTarget);
        continue;
      default:
        break;
      }
      if (!Last && !Signed && Native == macho_unwind::kARM64ModeFrameless &&
          readLE<uint32_t>(Source.data() + Offset) ==
              macho_arm64e_abi::kReturnToLR)
        continue;
      if (Last) {
        // Plain RET must select LR. RETAB has an implicit LR, SP and B key.
        if (Signed ? Insn.Id != AARCH64_INS_RETAB
                   : readLE<uint32_t>(Source.data() + Offset) !=
                         macho_arm64e_abi::kReturnToLR)
          return refuse(kLeafCode);
      } else if (!Offset && readLE<uint32_t>(Source.data()) ==
                                static_cast<uint32_t>(PAuthEncoding::PACIBSP)) {
        Signed = true;
      } else if (Body && (Offset < Body->Begin || Offset >= Body->End)) {
        // Both images performed the same frame operation at this offset.
        continue;
      } else if (!isPositionIndependentLeafInstruction(Insn)) {
        return refuse(kLeafCode);
      }
    }
    // A jump into the second of two exchanged writes executes only one of
    // them. Reordering preserves the entry state only along contiguous paths.
    if (llvm::any_of(BranchTargets, [&](va_t Target) {
          return llvm::is_contained(SwappedSeconds, Target);
        }))
      return refuse(kLeafCode);
  }
  if (!Addresses.empty())
    return refuse(kSourceAddressTarget);
  return llvm::Error::success();
}

// Insert words only in independently validated register bodies. Each offset
// refers to the original assembly, including when a later retry widens another
// site in the same owner. Update each function once per placement attempt.
bool widenSourceAddresses(
    llvm::Module &M, llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<va_t> PoolSites,
    llvm::MutableArrayRef<MachOLiftedSourceAddress> Sites) {
  bool Changed = false;
  while (!Mappings.empty() && !Sites.empty()) {
    const auto &Owner = Mappings.front();
    Mappings = Mappings.drop_front();
    const size_t Count =
        Mappings.empty() ? Sites.size()
                         : llvm::lower_bound(Sites, Mappings.front().OriginalVA,
                                             [](const auto &S, va_t VA) {
                                               return S.Instruction < VA;
                                             }) -
                               Sites.begin();
    auto Local = Sites.take_front(Count);
    Sites = Sites.drop_front(Count);
    llvm::SmallVector<size_t> Insertions;
    size_t Added = 0;
    for (auto &Site : Local) {
      if (Site.Form == MachOAddressForm::Expanded) {
        Added += llvm::StringRef(kAddressPrefix).size();
      } else if (Site.CanExpand && Site.Form == MachOAddressForm::Single &&
                 llvm::binary_search(PoolSites, Site.Instruction)) {
        Insertions.push_back(Site.AssemblyOffset + Added);
        Site.Form = MachOAddressForm::Expanded;
      }
    }
    if (Insertions.empty())
      continue;
    auto &F = *M.getFunction(Owner.Symbol);
    auto &Call = llvm::cast<llvm::CallInst>(F.front().front());
    auto *Asm = llvm::cast<llvm::InlineAsm>(Call.getCalledOperand());
    auto Text = Asm->getAsmString().str();
    for (auto Offset : llvm::reverse(Insertions))
      Text.insert(Offset, kAddressPrefix);
    Call.setCalledOperand(llvm::InlineAsm::get(
        Asm->getFunctionType(), Text, Asm->getConstraintString(),
        Asm->hasSideEffects(), Asm->isAlignStack(), Asm->getDialect(),
        Asm->canThrow()));
    Changed = true;
  }
  return Changed;
}

// A CFI row or language range may require the address use to stay one word.
// Move its whole function near the trailing pool, keeping all in-body offsets
// intact. The installer rechecks every call, unwind range and address there.
bool placeLiteralUsersLast(llvm::Module &M,
                           llvm::ArrayRef<SourcePatchMapping> Mappings,
                           llvm::ArrayRef<va_t> Sites) {
  llvm::DenseSet<llvm::Function *> Users;
  for (va_t PC : Sites) {
    while (Mappings.size() > 1 && Mappings[1].OriginalVA <= PC)
      Mappings = Mappings.drop_front();
    Users.insert(M.getFunction(Mappings.front().Symbol));
  }
  llvm::SmallVector<llvm::Function *> Ordered;
  bool Changed = false;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (Users.contains(&F))
      Ordered.push_back(&F);
    else if (!Ordered.empty())
      Changed = true;
  }
  if (Changed) {
    auto &Functions = M.getFunctionList();
    for (auto *F : Ordered)
      Functions.splice(Functions.end(), Functions, F->getIterator());
  }
  return Changed;
}
} // namespace

llvm::Error restoreMachOLiftedCompactFrames(
    const BinaryImage &Original, const BinaryImage &Layout,
    const llvm::DenseMap<va_t, const ExceptionFunction *> &Sources,
    const CompiledImage &Image, va_t Slide, va_t EHSectionVA,
    CompiledSection &EH,
    llvm::MutableArrayRef<NativeMachOCompactRange> Ranges) {
  const auto NeedsCompact = [&](const NativeMachOCompactRange &Range) {
    return (Range.Encoding & macho_unwind::kModeMask) ==
               macho_unwind::kARM64ModeDwarf &&
           Sources.contains(Range.BeginVA);
  };
  if (llvm::none_of(Ranges, NeedsCompact))
    return llvm::Error::success();

  const auto Frames = dwarf_eh::parseEHFrame(
      Layout, {EH.ExternalBytes.data(), EH.ExternalBytes.size(), EH.VA}, {});
  llvm::DenseMap<va_t, const DwarfFDE *> FDEs;
  for (const auto &Frame : Frames.FDEs)
    FDEs.try_emplace(Frame.InitialLocation, &Frame);
  Decoder Decode;
  if (!Decode.init(Original))
    return refuse(kLeafCode);
  llvm::DenseSet<va_t> Replaced;
  for (auto &Range : Ranges) {
    if (!NeedsCompact(Range))
      continue;
    const auto &Source = *Sources.lookup(Range.BeginVA);
    const auto &Compact = *Source.Compact;
    const auto *FDE = FDEs.lookup(Range.BeginVA);
    const auto Offset = *Compact.ReturnAddressSigningOffset;
    constexpr auto Word = aarch64::kInsnSize;
    if (Source.Dwarf || !FDE ||
        (Range.Encoding & macho_unwind::kDwarfSectionOffsetMask) !=
            EH.VA - EHSectionVA + FDE->SectionOffset ||
        !rangeInBounds(Offset, Word, Range.EndVA - Range.BeginVA) ||
        Range.BeginVA < Image.BaseVA ||
        !rangeInBounds(Range.BeginVA - Image.BaseVA,
                       Range.EndVA - Range.BeginVA, Image.Bytes.size()))
      return refuse(kLeafFrame);
    const auto *Sign =
        Image.Bytes.data() + (Range.BeginVA - Image.BaseVA) + Offset;
    DecodedInsn Insn{};
    if (Decode.decodeOne(Sign, Word, Range.BeginVA + Offset, Insn) != Word ||
        Insn.Id != AARCH64_INS_PACIBSPPC)
      return refuse(kLeafFrame);
    auto Rows = machOCompactFrameRows(Compact);
    Rows.push_back(
        {.Kind = CFIOpKind::NegateRAStateWithPC, .CodeOffset = Offset + Word});
    if (!sameCompactFrameRows(Source, Frames, FDE, Range, Slide, Rows,
                              kBKeyFrameAugmentation))
      return refuse(kLeafFrame);
    Range.Encoding = Compact.NativeEncoding & ~macho_unwind::kPersonalityMask;
    Replaced.insert(FDE->SectionOffset);
  }
  return retainIndexedFrames(EH, EHSectionVA, Frames, Replaced, Ranges);
}

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
  std::vector<MachODataInCodeRange> RecoveredData;
  std::vector<MachOLiftedSourceAddress> Addresses;
  llvm::DenseMap<va_t, ExceptionAddressRange> RetainedTails;
  auto RegisterEntries = validateMachORegisterResults(
      Module, Image, &RecoveredData, &Addresses, &RetainedTails);
  if (!RegisterEntries)
    return RegisterEntries.takeError();
  llvm::SmallVector<MachODataInCodeRange> Combined;
  llvm::ArrayRef<MachODataInCodeRange> SourceData = Image.MachODataInCode;
  if (!RecoveredData.empty()) {
    llvm::append_range(Combined, SourceData);
    llvm::append_range(Combined, RecoveredData);
    llvm::sort(Combined,
               [](const auto &L, const auto &R) { return L.Begin < R.Begin; });
    SourceData = Combined;
  }
  llvm::SmallVector<va_t> SectionEntries;
  for (const auto &S : Image.Sections) {
    if (!S.Size || !(S.Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                               llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)))
      continue;
    if (S.SegmentName == section_names::macho::TextSeg &&
        (S.Type & llvm::MachO::SECTION_TYPE) == llvm::MachO::S_SYMBOL_STUBS)
      continue;
    const auto *Segment = Image.getSegmentFor(S.VA);
    const auto Embedded = machODataInCodeRanges(SourceData, S.VA, S.Size);
    if ((S.Type & llvm::MachO::SECTION_TYPE) != llvm::MachO::S_REGULAR ||
        (!(S.Type & llvm::MachO::S_ATTR_PURE_INSTRUCTIONS) &&
         Embedded.empty()) ||
        !Segment || !Segment->isExecutable() || Segment->isWritable() ||
        S.VA % aarch64::kInsnSize || S.Size % aarch64::kInsnSize ||
        S.FileSz != S.Size || Image.vaToFileOffset(S.VA) != S.FileOff ||
        !rangeInBounds(S.VA - Segment->VA, S.Size, Segment->FileSz) ||
        !rangeInBounds(S.FileOff, S.Size, Image.Raw.size()))
      return llvm::createStringError(
          llvm::errc::not_supported,
          llvm::formatv(kSourceSection, S.SegmentName, S.Name).str());
    va_t First = S.VA;
    for (const auto &R : Embedded) {
      if (R.Begin != First)
        break;
      First = R.End;
    }
    if (First - S.VA < S.Size)
      SectionEntries.push_back(First);
  }
  if (SectionEntries.empty())
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
    if (F.isDeclaration() && !*Entry && F.use_empty())
      continue;
    if (F.isDeclaration() && *Entry && Image.findImportStubAt(**Entry)) {
      if (!F.use_empty())
        Bindings.push_back(
            {F.getName().str(), SourcePatchBindingKind::Function, **Entry, 0});
      continue;
    }
    if (F.isDeclaration() || !*Entry ||
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
  if (Mappings.empty() || std::adjacent_find(Mappings.begin(), Mappings.end(),
                                             [](const auto &L, const auto &R) {
                                               return L.OriginalVA ==
                                                      R.OriginalVA;
                                             }) != Mappings.end())
    return refuse(kLeafCoverage);

  const auto HasMapping = [&](va_t Address) {
    const auto It = std::lower_bound(
        Mappings.begin(), Mappings.end(), Address,
        [](const SourcePatchMapping &M, va_t VA) { return M.OriginalVA < VA; });
    return It != Mappings.end() && It->OriginalVA == Address;
  };
  const auto Missing = [&](va_t Address) {
    // A coalesced compact range can span a data export between code sections.
    // Only a callable entry needs a corresponding LLVM definition.
    return Image.hasAuthenticatedFunctionEntryAt(Address) &&
           !Image.isImportStubAt(Address) && !HasMapping(Address);
  };
  if (!llvm::all_of(SectionEntries, HasMapping) ||
      (Image.Entry && Missing(Image.Entry)) ||
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
  if (auto Error = bindLeafErrorResults(*Candidate, *RegisterEntries))
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
    llvm::LoopAnalysisManager Loops;
    llvm::FunctionAnalysisManager Analyses;
    llvm::CGSCCAnalysisManager CallGraph;
    llvm::ModuleAnalysisManager Modules;
    llvm::PassBuilder Passes;
    Passes.registerModuleAnalyses(Modules);
    Passes.registerCGSCCAnalyses(CallGraph);
    Passes.registerFunctionAnalyses(Analyses);
    Passes.registerLoopAnalyses(Loops);
    Passes.crossRegisterProxies(Loops, Analyses, CallGraph, Modules);
    llvm::FunctionPassManager Cleanup;
    Cleanup.addPass(llvm::DCEPass());
    Cleanup.addPass(llvm::SROAPass(llvm::SROAOptions::PreserveCFG));
    // AArch64 masks variable shift counts in LowIR. Drop the generic IR
    // overflow guard when its predicate is provably constant after SROA.
    Cleanup.addPass(llvm::InstSimplifyPass());
    Cleanup.addPass(llvm::DCEPass());
    for (auto &F : *Candidate) {
      if (F.isDeclaration())
        continue;
      Cleanup.run(F, Analyses);
      canonicalizeLeafOperations(F, Analyses);
      llvm::DCEPass().run(F, Analyses);
    }
  }
  const auto Publish = [&]() -> llvm::Expected<PatchResult> {
    // LLVM's emission passes mutate their module. Keep the verified assembly
    // and its byte offsets intact while a one-word site can require a retry.
    const bool MayRetry = llvm::any_of(Addresses, [](const auto &Site) {
      return Site.Form == MachOAddressForm::Single;
    });
    auto Copy = MayRetry ? llvm::CloneModule(*Candidate) : nullptr;
    auto Object =
        Codegen().compile(Copy ? *Copy : *Candidate, Image.Arch, Image.Format,
                          *Triple, ObjectConsumer::FinalImageInstaller);
    if (!Object.Success)
      return refuse(kLeafCode);
    return installMachOLiftedObject(
        llvm::StringRef(
            reinterpret_cast<const char *>(Object.ObjectData.data()),
            Object.ObjectData.size()),
        Image, Mappings, Bindings, Output, RecoveredData, Addresses,
        [&](const CompiledImage &Generated, const BinaryImage &Layout,
            va_t Slide, llvm::ArrayRef<NativeMachOCompactRange> Ranges,
            const CompiledSection *AddressPool) {
          return verifyLeaves(Image, Layout, Mappings, Generated, Slide, Ranges,
                              *RegisterEntries, RetainedTails, SourceData,
                              Addresses, AddressPool);
        });
  };
  bool Reordered = false;
  for (;;) {
    auto Result = Publish();
    if (Result)
      return Result;
    auto Error = Result.takeError();
    if (!Error.isA<MachOLiteralPoolOutOfRange>())
      return Error;
    std::vector<va_t> Sites;
    llvm::handleAllErrors(std::move(Error), [&](MachOLiteralPoolOutOfRange &E) {
      Sites = std::move(E.Instructions);
    });
    if (widenSourceAddresses(*Candidate, Mappings, Sites, Addresses))
      continue;
    if (!Reordered && placeLiteralUsersLast(*Candidate, Mappings, Sites)) {
      Reordered = true;
      continue;
    }
    return llvm::make_error<MachOLiteralPoolOutOfRange>(Sites);
  }
}
} // namespace neverd
