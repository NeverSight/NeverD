//===- MedLLVMAnalysisDispatch.cpp - Tentative physical targets -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A private source view compares the actual target against candidate addresses
/// and preserves an uninterpreted unknown exit. Native emission cannot use it.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMDispatchTargets.h"

#include "neverd/backend/LLVMAnalysisDispatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/MedAnalysisDispatch.h"

#include <map>

namespace neverd {

bool MedLLVMEmitter::emitAnalysisDispatch(
    const MedBlock &Block, const MedOp &Branch,
    const std::map<int, llvm::BasicBlock *> &BBMap,
    llvm::IRBuilder<> &Builder) {
  if (!AnalysisRecoveryForSource || !CurMedFunc || !CurMedFunc->AnalysisOnly)
    return false;
  const JumpTable *Table = nullptr;
  for (const auto &Candidate : CurMedFunc->JumpTables)
    if (Candidate.InsnAddr == Branch.Addr) {
      Table = &Candidate;
      break;
    }
  if (!Table || !Table->AnalysisOnly)
    return false;
  if (!hasAnalysisDispatchShape(true, Branch, *Table))
    throw std::runtime_error("malformed tentative analysis dispatch");
  const auto CaseBlocks =
      dispatchTargetBlocks(*CurMedFunc, Block, Table->Targets, BBMap);
  if (CaseBlocks.empty())
    throw std::runtime_error("tentative analysis dispatch lost a target");
  std::map<va_t, llvm::BasicBlock *> Targets;
  for (size_t I = 0; I != Table->Targets.size(); ++I)
    Targets.emplace(Table->Targets[I], CaseBlocks[I]);
  llvm::Value *Target = getVar(Branch.Inputs[0], Builder);
  if (!Target || !Target->getType()->isIntegerTy(32))
    throw std::runtime_error("tentative analysis dispatch has no target value");
  auto *Unknown = llvm::BasicBlock::Create(*Ctx, "unknown_target", CurFunc);
  auto *Switch = Builder.CreateSwitch(Target, Unknown, Targets.size());
  for (const auto &[Address, Block] : Targets)
    Switch->addCase(Builder.getInt32(Address), Block);
  llvm::IRBuilder<> Exit(Unknown);
  llvm_analysis_dispatch::create(Exit, Target, Branch.Addr);
  Exit.CreateUnreachable();
  return true;
}

} // namespace neverd
