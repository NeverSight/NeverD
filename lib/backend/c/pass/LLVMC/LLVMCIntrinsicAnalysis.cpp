//===- LLVMCIntrinsicAnalysis.cpp - Intrinsic struct analysis ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Intrinsic struct identification (CPUID, XGETBV) and call-result liveness
/// analysis for the LLVM-route C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"

namespace neverd {

bool isLinuxX64SyscallInlineAsm(const llvm::CallInst &Call) {
  const auto *IA = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!IA || IA->getAsmString() != "syscall" ||
      IA->getConstraintString() !=
          "={ax},={r11},0,{di},{si},{dx},{r10},{r8},{r9},~{rcx},~{memory},"
          "~{dirflag},~{fpsr},~{flags}")
    return false;
  const auto *Pair = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (!Pair || Pair->isOpaque() || Pair->getNumElements() != 2 ||
      !Pair->getElementType(0)->isIntegerTy(64) ||
      !Pair->getElementType(1)->isIntegerTy(64) || Call.arg_size() != 7)
    return false;
  for (const llvm::Use &Arg : Call.args())
    if (!Arg->getType()->isIntegerTy(64))
      return false;
  return true;
}

void analyzeIntrinsicStructs(LLVMCAnalysisState &State, llvm::Function &Fn) {
  State.IntrinsicStructVals.clear();
  State.IntrinsicStructNames.clear();

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst);
      if (!CI)
        continue;
      auto *IA = llvm::dyn_cast<llvm::InlineAsm>(CI->getCalledOperand());
      if (!IA)
        continue;
      if (!CI->getType()->isStructTy())
        continue;

      std::string AsmStr = IA->getAsmString().str();
      if (AsmStr == "cpuid") {
        State.IntrinsicStructVals.insert(CI);
        State.IntrinsicStructNames[CI] = "cpuInfo";
      } else if (AsmStr == "xgetbv") {
        State.IntrinsicStructVals.insert(CI);
        State.IntrinsicStructNames[CI] = "xcr";
      }
    }
  }
}

bool isCallResultLive(const LLVMCAnalysisState &State,
                      const llvm::CallInst *Call) {
  for (auto *U : Call->users()) {
    if (llvm::isa<llvm::ReturnInst>(U))
      continue;
    if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U))
      if (State.DeadFrameStores.count(UI))
        continue;
    return true;
  }
  return false;
}

} // namespace neverd
