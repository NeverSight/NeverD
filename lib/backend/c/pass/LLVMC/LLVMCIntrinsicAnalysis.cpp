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

#include <stdexcept>

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

bool isWindowsX64SyscallInlineAsm(const llvm::CallInst &Call) {
  const auto *IA = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!IA || IA->getAsmString() != "syscall" ||
      IA->getConstraintString() !=
          "={ax},={r11},={dx},={r8},={r9},={r10},0,2,3,4,5,~{rcx},~{memory},"
          "~{dirflag},~{fpsr},~{flags}")
    return false;
  const auto *Regs = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (!Regs || Regs->isOpaque() || Regs->getNumElements() != 6 ||
      Call.arg_size() != 5)
    return false;
  for (llvm::Type *Field : Regs->elements())
    if (!Field->isIntegerTy(64))
      return false;
  for (const llvm::Use &Arg : Call.args())
    if (!Arg->getType()->isIntegerTy(64))
      return false;
  return true;
}

unsigned x86CPUQueryResultWords(const llvm::CallInst &Call) {
  const auto *IA = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!IA)
    return 0;
  const llvm::StringRef Mnemonic = IA->getAsmString().trim().take_until(
      [](char C) { return C == ' ' || C == '\t' || C == '\n' || C == '\r'; });
  if (Mnemonic != "cpuid" && Mnemonic != "xgetbv")
    return 0;
  const bool XCR = Mnemonic == "xgetbv";
  const unsigned Words = XCR ? 2 : 4;
  const auto *Result = llvm::dyn_cast<llvm::StructType>(Call.getType());
  bool Valid =
      IA->getAsmString() == Mnemonic && IA->hasSideEffects() &&
      !IA->isAlignStack() && !IA->canThrow() &&
      IA->getDialect() == llvm::InlineAsm::AD_ATT &&
      IA->getConstraintString() ==
          (XCR ? "={eax},={edx},{ecx},~{memory}"
               : "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory}") &&
      Result && Result->isLiteral() && !Result->isPacked() &&
      Result->getNumElements() == Words && Call.arg_size() == (XCR ? 1u : 2u) &&
      !Call.hasOperandBundles();
  if (Valid) {
    for (auto *Field : Result->elements())
      Valid &= Field->isIntegerTy(32);
    for (const auto &Arg : Call.args())
      Valid &= Arg->getType()->isIntegerTy(32);
  }
  if (!Valid)
    throw std::runtime_error(
        "unsupported x86 CPU query inline assembly contract");
  return Words;
}

void analyzeIntrinsicStructs(LLVMCAnalysisState &State, llvm::Function &Fn) {
  State.IntrinsicStructVals.clear();
  State.IntrinsicStructNames.clear();

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst);
      if (!CI)
        continue;
      const unsigned Words = x86CPUQueryResultWords(*CI);
      if (!Words)
        continue;
      // The dedicated array projection represents register extracts, not an
      // arbitrary LLVM aggregate ABI. Refuse unrepresented aggregate uses.
      for (const auto *User : CI->users()) {
        const auto *Extract = llvm::dyn_cast<llvm::ExtractValueInst>(User);
        if (!Extract || Extract->getNumIndices() != 1 ||
            Extract->getIndices()[0] >= Words)
          throw std::runtime_error("unsupported x86 CPU query aggregate use");
      }
      State.IntrinsicStructVals.insert(CI);
      State.IntrinsicStructNames[CI] = Words == 4 ? "cpuInfo" : "xcr";
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
