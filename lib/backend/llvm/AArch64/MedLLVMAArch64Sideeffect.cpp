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

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#define DEBUG_TYPE "neverd-med-llvm-aarch64-sideeffect"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAArch64.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Memory barriers (DMB, DSB, ISB)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitAArch64Barrier(const MedOp & /*Op*/, Intrinsic IC,
                                        llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::Dmb:
    Builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    return true;
  case I::Dsb: {
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, "dsb ish", "~{memory}", true);
    Builder.CreateCall(IA, {});
    return true;
  }
  case I::Isb: {
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, "isb", "~{memory}", true);
    Builder.CreateCall(IA, {});
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
  case I::Hlt_A64: {
    // These instructions raise a synchronous exception and do not have an
    // architectural fallthrough path.  Use the noreturn trap intrinsic rather
    // than debugtrap; EH lowering removes the CFG's conservative fallthrough
    // edge after block emission, once any edge-copy bookkeeping is complete.
    Builder.CreateIntrinsic(llvm::Type::getVoidTy(*Ctx), llvm::Intrinsic::trap,
                            {});
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
      std::string AsmStr = std::string(Mn) + " #" + std::to_string(Imm);
      auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
      auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
      auto *IA = llvm::InlineAsm::get(AsmFnTy, AsmStr, "~{memory}", true);
      Builder.CreateCall(IA, {});
    }
    return true;
  }
  case I::A64_Clrex: {
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, "clrex", "~{memory}", true);
    Builder.CreateCall(IA, {});
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
  if (IC == Intrinsic::A64_SetFPSR) {
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
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::aarch64_set_fpsr);
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
