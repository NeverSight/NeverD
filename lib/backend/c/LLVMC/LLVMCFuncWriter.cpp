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

#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/Metadata.h"
#include "llvm/TargetParser/Triple.h"

namespace neverd {

namespace {

bool isKnownWindowsPersonality(llvm::StringRef Name) {
  Name.consume_front("#");
  return Name.contains("C_specific_handler") ||
         Name.contains("CxxFrameHandler") || Name.contains("GSHandlerCheck") ||
         Name.contains("except_handler") ||
         Name.ends_with("_personality_seh0") ||
         Name == "_GCC_specific_handler" || Name == "__gnat_personality_imp" ||
         Name == "ProcessCLRException";
}

bool isKnownWindowsPersonality(const llvm::Value *Personality) {
  llvm::SmallPtrSet<const llvm::Value *, 8> Visited;
  const llvm::Value *Current = Personality;
  while (Current) {
    Current = Current->stripPointerCasts();
    if (!Visited.insert(Current).second)
      return true;

    if (const auto *Named = llvm::dyn_cast<llvm::GlobalValue>(Current))
      if (isKnownWindowsPersonality(Named->getName()))
        return true;

    if (const auto *Alias = llvm::dyn_cast<llvm::GlobalAlias>(Current)) {
      Current = Alias->getAliasee();
      continue;
    }

    // An ifunc selects its implementation by executing arbitrary resolver
    // code.  C source cannot prove which personality it returns, so the only
    // safe executable projection is the analysis-only trap.
    if (llvm::isa<llvm::GlobalIFunc>(Current))
      return true;

    switch (llvm::classifyEHPersonality(Current)) {
    case llvm::EHPersonality::MSVC_X86SEH:
    case llvm::EHPersonality::MSVC_TableSEH:
    case llvm::EHPersonality::MSVC_CXX:
    case llvm::EHPersonality::CoreCLR:
      return true;
    default:
      return false;
    }
  }
  return false;
}

bool isWindowsEHIntrinsic(llvm::StringRef Name) {
  return Name == "llvm.eh.actions" || Name == "llvm.eh.exceptioncode" ||
         Name == "llvm.eh.recoverfp" || Name == "llvm.localescape" ||
         Name == "llvm.localrecover";
}

bool metadataReferencesFunction(
    const llvm::Metadata *MD, const llvm::Function &Fn,
    llvm::SmallPtrSetImpl<const llvm::Metadata *> &Visited) {
  if (!MD || !Visited.insert(MD).second)
    return false;
  if (const auto *ValueMD = llvm::dyn_cast<llvm::ValueAsMetadata>(MD))
    return ValueMD->getValue()->stripPointerCasts() == &Fn;
  const auto *Node = llvm::dyn_cast<llvm::MDNode>(MD);
  if (!Node)
    return false;
  for (const llvm::MDOperand &Operand : Node->operands())
    if (metadataReferencesFunction(Operand.get(), Fn, Visited))
      return true;
  return false;
}

void writeCommentedLine(llvm::raw_ostream &OS, llvm::StringRef Line) {
  OS << "// ";
  for (unsigned char Ch : Line.bytes()) {
    if (Ch == '\r' || Ch == '\0' || (Ch < 0x20 && Ch != '\t'))
      OS << ' ';
    else
      OS << static_cast<char>(Ch);
  }
  OS << " \n";
}

} // anonymous namespace

bool LLVMCWriter::isAnalysisOnlyFunction(const llvm::Function &Fn) const {
  if (Fn.getMetadata(windows_eh_md::FunctionAttachment) ||
      Fn.getMetadata(windows_eh_md::NativeAttachment))
    return true;

  if (const llvm::Module *Module = Fn.getParent()) {
    if (const llvm::NamedMDNode *Table =
            Module->getNamedMetadata(windows_eh_md::FunctionTable)) {
      for (const llvm::MDNode *Row : Table->operands()) {
        llvm::SmallPtrSet<const llvm::Metadata *, 16> Visited;
        if (metadataReferencesFunction(Row, Fn, Visited))
          return true;
      }
    }
  }

  bool HasWindowsIREvidence = false;
  for (const llvm::BasicBlock &BB : Fn) {
    for (const llvm::Instruction &Inst : BB) {
      HasWindowsIREvidence |=
          llvm::isa<llvm::CatchSwitchInst, llvm::CatchPadInst,
                    llvm::CatchReturnInst, llvm::CleanupPadInst,
                    llvm::CleanupReturnInst>(&Inst);
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
        HasWindowsIREvidence |= Call->countOperandBundlesOfType(
                                    windows_eh_md::ProvenanceBundle) != 0;
        if (const llvm::Function *Callee = Call->getCalledFunction())
          HasWindowsIREvidence |= isWindowsEHIntrinsic(Callee->getName());
      }
    }
  }
  if (HasWindowsIREvidence)
    return true;

  if (!Fn.hasPersonalityFn())
    return false;
  if (isKnownWindowsPersonality(Fn.getPersonalityFn()))
    return true;

  const llvm::Module *Module = Fn.getParent();
  return Module && !Module->getTargetTriple().empty() &&
         llvm::Triple(Module->getTargetTriple()).isOSWindows();
}

bool LLVMCWriter::isReferencedByExecutableProjection(
    const llvm::Function &Fn) const {
  llvm::SmallVector<const llvm::User *, 16> Worklist;
  llvm::SmallPtrSet<const llvm::User *, 32> Visited;
  for (const llvm::User *User : Fn.users())
    Worklist.push_back(User);

  while (!Worklist.empty()) {
    const llvm::User *User = Worklist.pop_back_val();
    if (!Visited.insert(User).second)
      continue;
    if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(User)) {
      const llvm::Function *Owner = Inst->getFunction();
      if (Owner && !isAnalysisOnlyFunction(*Owner))
        return true;
      continue;
    }
    if (const auto *Owner = llvm::dyn_cast<llvm::Function>(User)) {
      if (!Owner->isDeclaration() && !isAnalysisOnlyFunction(*Owner))
        return true;
      continue;
    }
    if (llvm::isa<llvm::GlobalAlias, llvm::GlobalIFunc>(User)) {
      for (const llvm::User *Next : User->users())
        Worklist.push_back(Next);
      continue;
    }
    if (llvm::isa<llvm::GlobalValue>(User))
      return true;
    for (const llvm::User *Next : User->users())
      Worklist.push_back(Next);
  }
  return false;
}

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
  if (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(Fn)) {
    writeAnalysisOnlyFunction(Fn);
    return;
  }
  writeFunctionProjection(Fn);
}

void LLVMCWriter::writeAnalysisOnlyFunction(llvm::Function &Fn) {
  if (Opts.EmitComments) {
    std::string Listing;
    llvm::raw_string_ostream ListingOS(Listing);
    LLVMCWriter ListingWriter(ListingOS, Opts, Dbg, Img,
                              /*GuardAnalysisOnlyFunctions=*/false);
    ListingWriter.GlobalIdentifierAllocator = GlobalIdentifierAllocator;
    ListingWriter.FunctionIdentifiers = FunctionIdentifiers;
    ListingWriter.writeFunction(Fn);
    ListingOS.flush();

    OS << "/* neverd.analysis-only: Windows EH semantics are not projected "
          "as executable C; the active definition traps. */\n";
    llvm::StringRef Remaining(Listing);
    while (!Remaining.empty()) {
      auto [Line, Rest] = Remaining.split('\n');
      writeCommentedLine(OS, Line);
      Remaining = Rest;
    }
  }

  std::string FName = functionIdentifier(Fn);
  llvm::FunctionType *FuncTy = Fn.getFunctionType();
  OS << typeToCLLVM(FuncTy->getReturnType()) << " " << FName << "(";
  CProjectionIdentifierAllocator ParameterIdentifiers;
  unsigned ParamIdx = 0;
  for (llvm::Argument &Arg : Fn.args()) {
    if (ParamIdx > 0)
      OS << ", ";
    std::string ParamName = "arg" + std::to_string(ParamIdx);
    if (Arg.hasName()) {
      std::string Raw = Arg.getName().str();
      if (!Raw.empty() && Raw != ParamName)
        ParamName = std::move(Raw);
    }
    OS << typeToCLLVM(Arg.getType()) << " "
       << ParameterIdentifiers.allocate(ParamName, "nd_arg");
    ++ParamIdx;
  }
  if (Fn.isVarArg()) {
    if (ParamIdx != 0)
      OS << ", ...";
  } else if (ParamIdx == 0) {
    OS << "void";
  }
  OS << ") {\n"
     << "    __builtin_trap();\n"
     << "}\n";
}

void LLVMCWriter::writeFunctionProjection(llvm::Function &Fn) {
  setupFunction(Fn);

  std::string FName = functionIdentifier(Fn);

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
  if (Fn.isVarArg() && ParamIdx != 0) {
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
