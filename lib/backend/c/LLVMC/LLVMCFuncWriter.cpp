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
#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/ExceptionEncoding.h"
#include "neverd/loader/ExceptionPersonality.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
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
      if (llvm::isa<llvm::CatchSwitchInst, llvm::CatchPadInst,
                    llvm::CleanupPadInst, llvm::CatchReturnInst,
                    llvm::CleanupReturnInst, llvm::InvokeInst>(&Inst))
        continue;
      if (Analysis.IntrinsicStructVals.count(&Inst))
        continue;
      Analysis.Inlinable.insert(&Inst);
    }
  }
}

bool LLVMCWriter::functionHasWindowsEHPads(const llvm::Function &Fn) const {
  for (const llvm::BasicBlock &BB : Fn) {
    const llvm::Instruction *Term = BB.getTerminator();
    if (Term && llvm::isa<llvm::CatchSwitchInst, llvm::CatchReturnInst,
                          llvm::CleanupReturnInst>(Term))
      return true;
    for (const llvm::Instruction &Inst : BB)
      if (llvm::isa<llvm::CatchPadInst, llvm::CleanupPadInst>(&Inst))
        return true;
  }
  return false;
}

bool LLVMCWriter::functionIsCxxEH(const llvm::Function &Fn) const {
  if (!Fn.hasPersonalityFn())
    return false;
  switch (llvm::classifyEHPersonality(Fn.getPersonalityFn())) {
  case llvm::EHPersonality::MSVC_CXX:
    return true;
  default:
    break;
  }
  if (const auto *GV = llvm::dyn_cast<llvm::GlobalValue>(
          Fn.getPersonalityFn()->stripPointerCasts())) {
    llvm::StringRef Name = GV->getName();
    return Name.contains("CxxFrameHandler") || Name.contains("CxxFrame");
  }
  return false;
}

void LLVMCWriter::writeExceptionAnnotation(const llvm::Function &Fn) {
  if (!Opts.EmitComments)
    return;
  const llvm::MDNode *Payload =
      Fn.getMetadata(windows_eh_md::FunctionAttachment);
  if (!Payload)
    Payload = Fn.getMetadata(windows_eh_md::NativeAttachment);
  if (!Payload)
    return;
  auto MdU64 = [&](unsigned Index) -> uint64_t {
    if (Index >= Payload->getNumOperands())
      return 0;
    if (const auto *C = llvm::dyn_cast<llvm::ConstantAsMetadata>(
            Payload->getOperand(Index)))
      if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(C->getValue()))
        return CI->getZExtValue();
    return 0;
  };
  auto MdStr = [&](unsigned Index) -> std::string {
    if (Index >= Payload->getNumOperands())
      return {};
    if (const auto *S =
            llvm::dyn_cast<llvm::MDString>(Payload->getOperand(Index)))
      return S->getString().str();
    return {};
  };
  OS << "/* neverd.exception: encoding="
     << getExceptionEncodingName(
            static_cast<ExceptionEncoding>(MdU64(windows_eh_md::Encoding)))
     << ", status="
     << getExceptionParseStatusName(static_cast<ExceptionParseStatus>(
            MdU64(windows_eh_md::ParseStatus)))
     << ", personality=" << MdStr(windows_eh_md::PersonalityName) << "\n";
  OS << " * code=[0x" << llvm::utohexstr(MdU64(windows_eh_md::CodeBegin))
     << ", 0x" << llvm::utohexstr(MdU64(windows_eh_md::CodeEnd)) << ")";
  if (uint64_t Unwind = MdU64(windows_eh_md::UnwindInfoVA))
    OS << ", unwind=0x" << llvm::utohexstr(Unwind);
  OS << " */\n";
}

void LLVMCWriter::setupFunction(llvm::Function &Fn) {
  NextVar = 0;
  ValNames.clear();
  UsedNames.clear();
  BlockLabels.clear();
  ReferencedBlocks.clear();
  EHTryDepth = 0;
  EHWrapIsCxx = false;

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
      // writeInstruction still emits `name = ...` for unused non-calls
      // (they are not inlinable when use_empty).  Skip only values that
      // the statement writer does not assign.
      if (Inst.getType()->isVoidTy() || Inst.getType()->isTokenTy())
        continue;
      if (llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (llvm::isa<llvm::CatchSwitchInst, llvm::CatchPadInst,
                    llvm::CleanupPadInst>(&Inst))
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
      if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst))
        if (isImportCalleeOnlyLoad(LI))
          continue;
      if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst))
        if (callDoesNotReturn(*CB))
          continue;
      if (InferredVoid) {
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst))
          if (!isCallResultLive(Analysis, CI))
            continue;
      }
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
    OS << "/* neverd.analysis-only: recovered Windows SEH/C++ as readable C. "
          "*/\n";
  }
  writeFunctionProjection(Fn);
}

void LLVMCWriter::writeFunctionProjection(llvm::Function &Fn) {
  setupFunction(Fn);

  std::string FName = functionIdentifier(Fn);
  writeExceptionAnnotation(Fn);

  CProjectionIdentifierAllocator ParameterIdentifiers;
  auto BindParam = [&](llvm::Argument &Arg, unsigned ParamIdx) {
    std::string Raw = Arg.hasName() ? Arg.getName().str() : std::string{};
    if (Raw.empty())
      Raw = "arg" + std::to_string(ParamIdx);
    std::string ParamName = ParameterIdentifiers.allocate(Raw, "nd_arg");
    ValNames[&Arg] = ParamName;
    UsedNames.insert(ParamName);
    return ParamName;
  };

  if (EmitFunctionWrapper) {
    auto *FuncTy = Fn.getFunctionType();
    std::string RetStr =
        InferredVoid ? "void" : typeToCLLVM(FuncTy->getReturnType());
    OS << RetStr << " " << FName << "(";

    unsigned ParamIdx = 0;
    for (auto &Arg : Fn.args()) {
      if (ParamIdx > 0)
        OS << ", ";
      OS << typeToCLLVM(Arg.getType()) << " " << BindParam(Arg, ParamIdx);
      ++ParamIdx;
    }
    if (Fn.isVarArg() && ParamIdx != 0) {
      if (ParamIdx > 0)
        OS << ", ";
      OS << "...";
    }
    OS << ") {\n";
  } else {
    unsigned ParamIdx = 0;
    for (auto &Arg : Fn.args()) {
      BindParam(Arg, ParamIdx);
      ++ParamIdx;
    }
  }

  emitFunctionDecls(Fn);

  const bool WrapEH =
      (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(Fn)) ||
      functionHasWindowsEHPads(Fn);
  EHWrapIsCxx = functionIsCxxEH(Fn);
  if (WrapEH) {
    emitIndent(1);
    OS << (EHWrapIsCxx ? "try {\n" : "__try {\n");
    EHTryDepth = 1;
  }

  for (auto &BB : Fn) {
    if (&BB != &Fn.getEntryBlock() && llvm::pred_empty(&BB))
      continue;
    AfterCxxThrow = false;
    if (!isSimpleEntry(&BB, Fn))
      OS << blockLabel(&BB) << ":\n";

    for (auto &Inst : BB) {
      if (llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      writeInstruction(Inst, 1 + EHTryDepth);
    }
  }

  if (WrapEH && EHTryDepth > 0) {
    emitIndent(1);
    if (EHWrapIsCxx)
      OS << "} catch (...) {\n";
    else
      OS << "} __except (EXCEPTION_EXECUTE_HANDLER) {\n";
    emitIndent(2);
    OS << "/* recovered handler labels remain in the protected body */\n";
    emitIndent(1);
    OS << "}\n";
    EHTryDepth = 0;
  }

  if (EmitFunctionWrapper)
    OS << "}\n";
}

} // namespace neverd
