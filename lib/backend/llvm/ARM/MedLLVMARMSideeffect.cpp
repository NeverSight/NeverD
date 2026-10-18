//===- MedLLVMARMSideeffect.cpp - ARM (32-bit) side-effect intrinsics -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM (32-bit) side-effect intrinsic emission: barriers (DMB/DSB/ISB),
/// supervisor calls, breakpoints, and exclusive monitors.
///
/// Value-producing intrinsics (RBIT) live in MedLLVMARMValueEmitter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/MedLLVMInstructionText.h"

#define DEBUG_TYPE "neverd-med-llvm-arm-sideeffect"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Memory barriers (DMB, DSB, ISB)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitARMBarrier(const MedOp &Op, Intrinsic IC,
                                    llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::ArmDmb:
    // The strongest fence orders what any DMB option does; the option is
    // the text it carries (`dmb ishst`).
    markInstructionText(
        *Builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent), Op,
        IC);
    return true;
  case I::ArmDsb:
  case I::ArmIsb: {
    // A DSB or ISB runs with its own option: `dsb sy` waits for more than
    // `dsb ish` does.  A lift that kept none ran the inner-shareable one.
    std::string Text = intrinsicInstructionText(IC, instructionOperand(Op));
    if (Text.empty())
      Text = IC == I::ArmDsb ? "dsb ish" : "isb";
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, Text, "~{memory}", true);
    Builder.CreateCall(IA, {});
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Exception generators (SVC, HVC, SMC, BKPT, HLT, UDF, CLREX)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitARMException(const MedOp &Op, Intrinsic IC,
                                      llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::ArmSvc:
  case I::ArmHvc:
  case I::ArmSmc:
  case I::ArmBkpt:
  case I::ArmHlt:
  case I::ArmUdf: {
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
  case I::ArmClrex: {
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
// Top-level ARM side-effect dispatch
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitARMSideeffect(const MedOp &Op, Intrinsic IC,
                                       llvm::IRBuilder<> &Builder) {
  if (emitARMBarrier(Op, IC, Builder))
    return true;
  if (emitARMException(Op, IC, Builder))
    return true;
  return false;
}

} // namespace neverd
