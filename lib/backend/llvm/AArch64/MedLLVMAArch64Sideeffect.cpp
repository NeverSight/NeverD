//===- MedLLVMAArch64Sideeffect.cpp - AArch64 side-effect intrinsics -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64-specific side-effect intrinsic emission: barriers (DMB/DSB/ISB),
/// hints (yield/wfe/wfi/sev/sevl), debug traps, and exception generators.
///
/// Value-producing intrinsics (RBIT) live in MedLLVMAArch64ValueEmitter.cpp.
///
//===----------------------------------------------------------------------===//

#include "AArch64SVEPredicate.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/MedLLVMInstructionText.h"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/ir/intrinsics/IntrinsicTermination.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#define DEBUG_TYPE "neverd-med-llvm-aarch64-sideeffect"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAArch64.h"

#include <stdexcept>

namespace neverd {

namespace {
void requireSVE(llvm::IRBuilder<> &Builder) {
  llvm::Function *Function = Builder.GetInsertBlock()->getParent();
  std::string Features =
      Function->getFnAttribute("target-features").getValueAsString().str();
  if (!llvm::StringRef(Features).contains("+sve")) {
    if (!Features.empty())
      Features += ',';
    Function->addFnAttr("target-features", Features + "+sve");
  }
}
} // namespace

//===----------------------------------------------------------------------===//
// Memory barriers (DMB, DSB, ISB)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitAArch64Barrier(const MedOp &Op, Intrinsic IC,
                                        llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::Dmb:
    // The strongest fence orders what any DMB option does; the option is
    // the text it carries (`dmb ishld`).
    markInstructionText(
        *Builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent), Op,
        IC);
    return true;
  case I::Dsb:
  case I::Isb: {
    // A DSB or ISB runs with its own option: `dsb sy` waits for more than
    // `dsb ish` does.  A lift that kept none ran the inner-shareable one.
    std::string Text = intrinsicInstructionText(IC, instructionOperand(Op));
    if (Text.empty())
      Text = IC == I::Dsb ? "dsb ish" : "isb";
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, Text, "~{memory}", true);
    Builder.CreateCall(IA, {});
    return true;
  }
  case I::A64_Wrffr:
  case I::A64_Rdffr:
  case I::A64_Setffr: {
    requireSVE(Builder);
    // The FFR transfers run as their instructions (`wrffr p0.b`), as HighC
    // prints them.
    const bool Set = IC == I::A64_Setffr;
    const auto Predicate = instructionOperand(Op);
    if (Op.Output.Size || !Op.IntrinsicOutputs.empty() ||
        Op.NumInputs != (Set ? 1 : 2) ||
        (!Set && (!Predicate || *Predicate >= 16)))
      throw std::invalid_argument("malformed AArch64 FFR transfer");
    const std::string Text =
        Set ? "setffr" : intrinsicInstructionText(IC, Predicate);
    const std::string Clobber =
        IC == I::A64_Rdffr ? "p" + std::to_string(*Predicate) : std::string();
    auto *AsmFnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), false);
    Builder.CreateCall(llvm::InlineAsm::get(
        AsmFnTy, Text,
        Clobber.empty() ? "~{memory}" : "~{" + Clobber + "},~{memory}", true));
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Hint instructions (YIELD, WFE, WFI, SEV, SEVL)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitAArch64Hint(const MedOp & /*Op*/, Intrinsic IC,
                                     llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::Yield_A64:
  case I::Wfe:
  case I::Wfi:
  case I::Sev:
  case I::Sevl: {
    const char *Hints[] = {"yield", "wfe", "wfi", "sev", "sevl"};
    int Idx = static_cast<int>(IC) - static_cast<int>(I::Yield_A64);
    if (Idx >= 0 && Idx < 5) {
      auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
      auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
      auto *IA = llvm::InlineAsm::get(AsmFnTy, Hints[Idx], "", true);
      Builder.CreateCall(IA, {});
    }
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Debug traps and exception generators (BRK, HLT, SVC, HVC, SMC, CLREX)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitAArch64Exception(const MedOp &Op, Intrinsic IC,
                                          llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::Brk:
  case I::Hlt_A64:
  case I::Udf_A64: {
    // A compiler debug break resumes without defining X0. Fatal traps retain
    // noreturn emission; all IR stages use the same instruction-code policy.
    const auto Kind = intrinsicNeverReturns(IC, instructionOperand(Op))
                          ? llvm::Intrinsic::trap
                          : llvm::Intrinsic::debugtrap;
    markInstructionText(
        *Builder.CreateIntrinsic(llvm::Type::getVoidTy(*Ctx), Kind, {}), Op,
        IC);
    return true;
  }
  case I::Eret: {
    // A terminal transfer to ELR with SPSR as PSTATE, as x86's IRET: a named
    // no-return call, so LLVMC prints the same helper as HighC and nothing
    // resumes after it.
    auto *FnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), false);
    llvm::FunctionCallee Callee =
        Mod->getOrInsertFunction(intrinsicCName(IC), FnTy);
    if (auto *Fn = llvm::dyn_cast<llvm::Function>(Callee.getCallee()))
      Fn->addFnAttr(llvm::Attribute::NoReturn);
    Builder.CreateCall(Callee)->setDoesNotReturn();
    return true;
  }
  case I::Svc:
  case I::Hvc:
  case I::Smc: {
    const char *Mn = intrinsicAsmMnemonic(IC);
    if (Mn) {
      uint64_t Imm = 0;
      for (int K = 1; K < Op.NumInputs; ++K)
        if (Op.Inputs[K].isConst()) {
          Imm = Op.Inputs[K].ConstVal;
          break;
        }
      // The instruction with its code (`udf #0xFE`); a supervisor call keeps
      // the decimal number it has always printed.
      std::string AsmStr = intrinsicInstructionText(IC, Imm);
      if (AsmStr.empty())
        AsmStr = std::string(Mn) + " #" + std::to_string(Imm);
      auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
      auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
      auto *IA = llvm::InlineAsm::get(AsmFnTy, AsmStr, "~{memory}", true);
      Builder.CreateCall(IA, {});
    }
    return true;
  }
  case I::A64_Clrex: {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::aarch64_clrex);
    Builder.CreateCall(Fn, {});
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Top-level AArch64 side-effect dispatch
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitAArch64Sideeffect(const MedOp &Op, Intrinsic IC,
                                           llvm::IRBuilder<> &Builder) {
  if (isAArch64AddressStoreIntrinsic(IC)) {
    // Original memory shares one validated machine contract. A void store
    // must reach it before the generic dispatcher refuses unknown effects.
    (void)emitAArch64IntrinsicValue(Op, IC, Builder);
    return true;
  }
  // An MSR, or a SYS operation (DC, IC, AT, TLBI), the lift kept the field
  // of runs as itself, with its register operand; it writes no register.
  if (Op.Output.Size == 0 && (Op.NumInputs == 2 || Op.NumInputs == 3) &&
      Op.Inputs[1].isConst()) {
    const bool HasRegister = Op.NumInputs == 3;
    const std::string Asm = aarch64SystemInstructionAsm(
        IC, Op.Inputs[1].ConstVal, /*HasOutput=*/false, HasRegister);
    if (!Asm.empty()) {
      llvm::SmallVector<llvm::Value *, 1> Arguments;
      if (HasRegister)
        Arguments.push_back(Builder.CreateZExtOrTrunc(
            getVar(Op.Inputs[2], Builder), Builder.getInt64Ty()));
      auto *Type = llvm::FunctionType::get(
          Builder.getVoidTy(),
          HasRegister ? llvm::ArrayRef<llvm::Type *>(Builder.getInt64Ty())
                      : llvm::ArrayRef<llvm::Type *>(),
          false);
      std::string Constraints = HasRegister ? "r,~{memory}" : "~{memory}";
      if (IC == Intrinsic::A64_SveRegisterTransfer) {
        requireSVE(Builder);
        const std::string Clobber =
            aarch64SveRegisterTransferClobber(Op.Inputs[1].ConstVal);
        if (!Clobber.empty())
          Constraints += ",~{" + Clobber + "}";
      }
      Builder.CreateCall(llvm::InlineAsm::get(Type, Asm, Constraints,
                                              /*hasSideEffects=*/true),
                         Arguments);
      return true;
    }
  }
  if (IC == Intrinsic::A64_SveSt1 || IC == Intrinsic::A64_SveStnt1) {
    if (!aarch64VectorMedShapeIsValid(Op))
      llvm::report_fatal_error(aarch64_sve::kMalformedVector);
    const unsigned LaneBytes = Op.Inputs[4].ConstVal;
    const unsigned StoreBytes = Op.Inputs[5].ConstVal;

    const unsigned MinLanes = 16 / LaneBytes;
    auto *PredTy =
        llvm::ScalableVectorType::get(llvm::Type::getInt1Ty(*Ctx), MinLanes);
    auto *SourceElemTy = llvm::IntegerType::get(*Ctx, LaneBytes * 8);
    auto *SourceTy = llvm::ScalableVectorType::get(SourceElemTy, MinLanes);
    auto *StoreElemTy = llvm::IntegerType::get(*Ctx, StoreBytes * 8);
    auto *StoreTy = llvm::ScalableVectorType::get(StoreElemTy, MinLanes);

    auto stateToScalable = [&](const MedVar &State, llvm::Type *ScalableTy,
                               llvm::StringRef Name) -> llvm::Value * {
      llvm::Value *Value = getVar(State, Builder);
      auto *Slot =
          Builder.CreateAlloca(Value->getType(), nullptr, Name + ".slot");
      Slot->setAlignment(llvm::Align(16));
      Builder.CreateStore(Value, Slot)->setAlignment(llvm::Align(16));
      return Builder.CreateLoad(ScalableTy, Slot, Name);
    };

    llvm::Value *Pred = stateToScalable(
        Op.Inputs[1], aarch64_sve::svboolType(*Ctx), "sve.store.pred");
    Pred = aarch64_sve::fromSVBool(Builder, Pred, PredTy);
    llvm::Value *Value =
        stateToScalable(Op.Inputs[2], SourceTy, "sve.store.value");
    if (SourceTy != StoreTy)
      Value = Builder.CreateTrunc(Value, StoreTy, "sve.store.narrow");

    llvm::Value *Address = getVar(Op.Inputs[3], Builder);
    auto *PtrTy = llvm::PointerType::getUnqual(*Ctx);
    if (!Address->getType()->isPointerTy())
      Address = Builder.CreateIntToPtr(Address, PtrTy);
    auto IID = IC == Intrinsic::A64_SveStnt1
                   ? llvm::Intrinsic::aarch64_sve_stnt1
                   : llvm::Intrinsic::aarch64_sve_st1;
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, IID, {StoreTy, Address->getType()});
    Builder.CreateCall(Fn, {Value, Pred, Address});
    return true;
  }
  if (IC == Intrinsic::Stg) {
    if (Op.NumInputs < 3)
      return true;
    auto *PtrTy = llvm::PointerType::getUnqual(*Ctx);
    auto asPointer = [&](const MedVar &Input) -> llvm::Value * {
      llvm::Value *Value = getVar(Input, Builder);
      if (Value->getType()->isPointerTy())
        return Value;
      return Builder.CreateIntToPtr(Value, PtrTy);
    };
    llvm::Value *TagSource = asPointer(Op.Inputs[1]);
    llvm::Value *Address = asPointer(Op.Inputs[2]);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::aarch64_stg);
    Builder.CreateCall(Fn, {TagSource, Address});
    return true;
  }
  if (IC == Intrinsic::A64_SetFPSR || IC == Intrinsic::A64_SetFPCR) {
    if (Op.NumInputs < 2)
      return true;
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    llvm::Value *Value = getVar(Op.Inputs[1], Builder);
    if (Value->getType()->isPointerTy())
      Value = Builder.CreatePtrToInt(Value, I64Ty);
    else if (Value->getType()->isFloatingPointTy()) {
      auto *BitsTy = llvm::IntegerType::get(
          *Ctx, Value->getType()->getPrimitiveSizeInBits());
      Value = Builder.CreateBitCast(Value, BitsTy);
    }
    if (Value->getType() != I64Ty)
      Value = Builder.CreateZExtOrTrunc(Value, I64Ty);
    llvm::Intrinsic::ID IID = IC == Intrinsic::A64_SetFPSR
                                  ? llvm::Intrinsic::aarch64_set_fpsr
                                  : llvm::Intrinsic::aarch64_set_fpcr;
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
    Builder.CreateCall(Fn, {Value});
    return true;
  }
  if (emitAArch64Barrier(Op, IC, Builder))
    return true;
  if (emitAArch64Hint(Op, IC, Builder))
    return true;
  if (emitAArch64Exception(Op, IC, Builder))
    return true;
  return false;
}

} // namespace neverd
