//===- LLVMCFuncWriter.cpp - LLVM IR function-level rendering --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function-level orchestration for the LLVM IR C emitter: analysis pass
/// scheduling, local variable declaration emission, block scanning, and
/// function signature rendering.  Instruction-level rendering lives in
/// LLVMCStmtWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "LLVMCWriter.h"

namespace neverd {

void LLVMCWriter::scanReferencedBlocks(llvm::Function &Fn) {
  ReferencedBlocks.clear();
  for (auto &BB : Fn) {
    auto *Term = BB.getTerminator();
    if (llvm::isa<llvm::UncondBrInst, llvm::CondBrInst>(Term)) {
      for (unsigned I = 0; I < Term->getNumSuccessors(); ++I)
        ReferencedBlocks.insert(Term->getSuccessor(I));
    } else if (auto *SW = llvm::dyn_cast<llvm::SwitchInst>(Term)) {
      ReferencedBlocks.insert(SW->getDefaultDest());
      for (auto &C : SW->cases())
        ReferencedBlocks.insert(C.getCaseSuccessor());
    }
  }
}

void LLVMCWriter::markInlinable(llvm::Function &Fn) {
  Analysis.Inlinable.clear();
  InlineCache.clear();
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (Inst.getType()->isVoidTy())
        continue;
      if (!Inst.hasOneUse())
        continue;
      if (llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (llvm::isa<llvm::CallInst>(&Inst))
        continue;
      if (llvm::isa<llvm::LoadInst>(&Inst))
        continue;
      if (llvm::isa<llvm::PHINode>(&Inst))
        continue;
      if (llvm::isa<llvm::ExtractValueInst>(&Inst))
        continue;
      if (Analysis.IntrinsicStructVals.count(&Inst))
        continue;
      Analysis.Inlinable.insert(&Inst);
    }
  }
}

void LLVMCWriter::setupFunction(llvm::Function &Fn) {
  NextVar = 0;
  ValNames.clear();
  UsedNames.clear();
  BlockLabels.clear();
  ReferencedBlocks.clear();

  scanReferencedBlocks(Fn);
  analyzeIntrinsicStructs(Analysis, Fn);
  markInlinable(Fn);
  analyzeDeadFrameStores(Analysis, Fn);
  analyzeStoreForwarding(Analysis, Fn);
  InferredVoid = analyzeVoidReturn(Analysis, Fn);

  if (InferredVoid)
    analyzeVoidDeadChain(Analysis, Fn);
}

void LLVMCWriter::emitFunctionDecls(llvm::Function &Fn) {
  std::set<const llvm::Value *> NeedsDecl;
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (Inst.getType()->isVoidTy())
        continue;
      if (Inst.use_empty() && !llvm::isa<llvm::CallInst>(&Inst))
        continue;
      if (llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (Analysis.IntrinsicStructVals.count(&Inst))
        continue;
      if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(&Inst))
        if (Analysis.IntrinsicStructNames.count(EV->getAggregateOperand()))
          continue;
      if (Analysis.Inlinable.count(&Inst))
        continue;
      if (Analysis.DeadFrameStores.count(&Inst))
        continue;
      if (InferredVoid && llvm::isa<llvm::CallInst>(&Inst) &&
          !isCallResultLive(Analysis, llvm::cast<llvm::CallInst>(&Inst)))
        continue;
      NeedsDecl.insert(&Inst);
    }
  }

  for (auto *V : NeedsDecl) {
    auto DeclName = getName(V);
    emitIndent(1);
    OS << typeToCLLVM(V->getType()) << " " << DeclName << ";\n";
  }

  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst)) {
        if (Analysis.DeadFrameAllocas.count(AI))
          continue;
        auto AllocName = getName(AI);
        emitIndent(1);
        auto *AllocTy = AI->getAllocatedType();
        if (auto *ArrTy = llvm::dyn_cast<llvm::ArrayType>(AllocTy)) {
          OS << typeToCLLVM(ArrTy->getElementType()) << " " << AllocName << "["
             << ArrTy->getNumElements() << "];\n";
        } else {
          OS << typeToCLLVM(AllocTy) << " " << AllocName << ";\n";
        }
      }
    }
  }

  if (!NeedsDecl.empty())
    OS << "\n";
}

void LLVMCWriter::writeFunction(llvm::Function &Fn) {
  setupFunction(Fn);

  std::string FName = Fn.getName().str();
  if (!FName.empty() && FName[0] == '_')
    FName = FName.substr(1);

  auto *FuncTy = Fn.getFunctionType();
  std::string RetStr =
      InferredVoid ? "void" : typeToCLLVM(FuncTy->getReturnType());
  OS << RetStr << " " << FName << "(";

  unsigned ParamIdx = 0;
  for (auto &Arg : Fn.args()) {
    if (ParamIdx > 0)
      OS << ", ";
    std::string ParamName = "arg" + std::to_string(ParamIdx);
    if (Arg.hasName()) {
      std::string Raw = Arg.getName().str();
      if (!Raw.empty() && Raw != "arg" + std::to_string(ParamIdx))
        ParamName = Raw;
    }
    ValNames[&Arg] = ParamName;
    UsedNames.insert(ParamName);
    OS << typeToCLLVM(Arg.getType()) << " " << ParamName;
    ++ParamIdx;
  }
  if (Fn.isVarArg()) {
    if (ParamIdx > 0)
      OS << ", ";
    OS << "...";
  }
  OS << ") {\n";

  emitFunctionDecls(Fn);

  for (auto &BB : Fn) {
    if (!isSimpleEntry(&BB, Fn))
      OS << blockLabel(&BB) << ":\n";

    for (auto &Inst : BB) {
      if (llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      writeInstruction(Inst, 1);
    }
  }

  OS << "}\n";
}

} // namespace neverd
