//===- LLVMCStmtWriter.cpp - LLVM IR instruction rendering ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction rendering for the LLVM IR C emitter: converts individual
/// LLVM instructions into C source lines.  Function-level orchestration
/// lives in LLVMCFuncWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "LLVMCWriter.h"

#include "llvm/IR/IntrinsicsAArch64.h"

namespace neverd {

void LLVMCWriter::emitIndent(int N) { emitCIndent(OS, N); }

void LLVMCWriter::writeInstruction(llvm::Instruction &Inst, int Indent) {
  if (llvm::isa<llvm::DbgInfoIntrinsic>(&Inst))
    return;
  if (Analysis.Inlinable.count(&Inst))
    return;
  if (Analysis.DeadFrameStores.count(&Inst))
    return;

  auto Name = Inst.getType()->isVoidTy() ? "" : getName(&Inst);

  if (Inst.isBinaryOp()) {
    auto LHS = valueStr(Inst.getOperand(0));
    auto RHS = valueStr(Inst.getOperand(1));
    emitIndent(Indent);
    OS << Name << " = " << binopStr(Inst.getOpcode(), LHS, RHS, Inst.getType())
       << ";\n";
    return;
  }

  if (auto *CI = llvm::dyn_cast<llvm::ICmpInst>(&Inst)) {
    auto LHS = valueStr(CI->getOperand(0));
    auto RHS = valueStr(CI->getOperand(1));
    emitIndent(Indent);
    OS << Name << " = " << cmpStr(CI->getPredicate(), LHS, RHS, false) << ";\n";
    return;
  }

  if (auto *FCI = llvm::dyn_cast<llvm::FCmpInst>(&Inst)) {
    auto LHS = valueStr(FCI->getOperand(0));
    auto RHS = valueStr(FCI->getOperand(1));
    emitIndent(Indent);
    OS << Name << " = " << cmpStr(FCI->getPredicate(), LHS, RHS, true) << ";\n";
    return;
  }

  if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = *(" << typeToCLLVM(LI->getType()) << "*)"
       << valueStr(LI->getPointerOperand()) << ";\n";
    return;
  }

  if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
    emitIndent(Indent);
    OS << "*(" << typeToCLLVM(SI->getValueOperand()->getType()) << "*)"
       << valueStr(SI->getPointerOperand()) << " = "
       << valueStr(SI->getValueOperand()) << ";\n";
    return;
  }

  if (llvm::isa<llvm::AllocaInst>(&Inst))
    return;

  if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&Inst)) {
    writeGEP(*GEP, Name, Indent);
    return;
  }

  if (Inst.isCast()) {
    auto Src = valueStr(Inst.getOperand(0));
    emitIndent(Indent);
    OS << Name << " = "
       << castStr(Inst.getOpcode(), Src, Inst.getOperand(0)->getType(),
                  Inst.getType())
       << ";\n";
    return;
  }

  if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst)) {
    writeCall(*Call, Name, Indent);
    return;
  }

  if (auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(&Inst)) {
    emitIndent(Indent);
    OS << "goto " << blockLabel(Br->getSuccessor(0)) << ";\n";
    return;
  }

  if (auto *Br = llvm::dyn_cast<llvm::CondBrInst>(&Inst)) {
    emitIndent(Indent);
    OS << "if (" << valueStr(Br->getCondition()) << ") goto "
       << blockLabel(Br->getSuccessor(0)) << "; else goto "
       << blockLabel(Br->getSuccessor(1)) << ";\n";
    return;
  }

  if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&Inst)) {
    writeReturn(*Ret, Indent);
    return;
  }

  if (auto *SW = llvm::dyn_cast<llvm::SwitchInst>(&Inst)) {
    emitIndent(Indent);
    OS << "switch (" << valueStr(SW->getCondition()) << ") {\n";
    for (auto &C : SW->cases()) {
      emitIndent(Indent);
      OS << "case " << C.getCaseValue()->getSExtValue() << ": goto "
         << blockLabel(C.getCaseSuccessor()) << ";\n";
    }
    emitIndent(Indent);
    OS << "default: goto " << blockLabel(SW->getDefaultDest()) << ";\n";
    emitIndent(Indent);
    OS << "}\n";
    return;
  }

  if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(&Inst)) {
    auto *Agg = EV->getAggregateOperand();
    unsigned Idx = EV->getIndices()[0];
    auto It = Analysis.IntrinsicStructNames.find(Agg);
    if (It != Analysis.IntrinsicStructNames.end())
      return;
    emitIndent(Indent);
    OS << Name << " = " << valueStr(Agg) << ".field_" << Idx << ";\n";
    return;
  }

  if (llvm::isa<llvm::PHINode>(&Inst)) {
    emitIndent(Indent);
    OS << "/* phi: " << Name << " */\n";
    return;
  }

  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = " << valueStr(Sel->getCondition()) << " ? "
       << valueStr(Sel->getTrueValue()) << " : "
       << valueStr(Sel->getFalseValue()) << ";\n";
    return;
  }

  if (auto *FI = llvm::dyn_cast<llvm::FenceInst>(&Inst)) {
    emitIndent(Indent);
    OS << renderFence(Opts.TheArch, FI->getOrdering());
    HasCIntrinsics = true;
    return;
  }

  if (llvm::isa<llvm::UnreachableInst>(&Inst)) {
    emitIndent(Indent);
    OS << "__builtin_unreachable();\n";
    return;
  }

  emitIndent(Indent);
  OS << "/* unhandled: ";
  std::string OpName;
  llvm::raw_string_ostream RSO(OpName);
  Inst.print(RSO);
  OS << OpName << " */\n";
}

void LLVMCWriter::writeGEP(llvm::GetElementPtrInst &GEP,
                           const std::string &Name, int Indent) {
  emitIndent(Indent);
  auto Base = valueStr(GEP.getPointerOperand());
  if (GEP.getNumIndices() == 1) {
    OS << Name << " = (void*)((char*)" << Base << " + "
       << valueStr(GEP.getOperand(1)) << ");\n";
  } else if (GEP.getNumIndices() == 2) {
    auto Idx0 = valueStr(GEP.getOperand(1));
    auto Idx1 = valueStr(GEP.getOperand(2));
    if (Idx0 == "0") {
      auto *SrcTy = GEP.getSourceElementType();
      if (auto *AT = llvm::dyn_cast<llvm::ArrayType>(SrcTy)) {
        OS << Name << " = &((" << typeToCLLVM(AT->getElementType()) << "*)"
           << Base << ")[" << Idx1 << "];\n";
      } else if (auto *ST = llvm::dyn_cast<llvm::StructType>(SrcTy)) {
        if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(GEP.getOperand(2))) {
          OS << Name << " = &((" << llvmStructName(ST) << "*)" << Base
             << ")->field_" << CI->getZExtValue() << ";\n";
        } else {
          OS << Name << " = (void*)((char*)" << Base << " + " << Idx1 << ");\n";
        }
      } else {
        OS << Name << " = (void*)((char*)" << Base << " + " << Idx1 << ");\n";
      }
    } else {
      OS << Name << " = (void*)((char*)" << Base << " + " << Idx0 << " + "
         << Idx1 << ");\n";
    }
  } else {
    OS << Name << " = (void*)" << Base << "; /* complex GEP */\n";
  }
}

bool LLVMCWriter::writeIntrinsicCall(llvm::CallInst &Call, int Indent) {
  auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(&Call);
  if (!II)
    return false;

  auto IID = II->getIntrinsicID();
  if (IID == llvm::Intrinsic::memcpy || IID == llvm::Intrinsic::memmove) {
    emitIndent(Indent);
    OS << "memcpy(" << valueStr(Call.getArgOperand(0)) << ", "
       << valueStr(Call.getArgOperand(1)) << ", "
       << valueStr(Call.getArgOperand(2)) << ");\n";
    return true;
  }
  if (IID == llvm::Intrinsic::memset) {
    emitIndent(Indent);
    OS << "memset(" << valueStr(Call.getArgOperand(0)) << ", "
       << valueStr(Call.getArgOperand(1)) << ", "
       << valueStr(Call.getArgOperand(2)) << ");\n";
    return true;
  }
  if (IID == llvm::Intrinsic::lifetime_start ||
      IID == llvm::Intrinsic::lifetime_end)
    return true;
  if (IID == llvm::Intrinsic::debugtrap) {
    emitIndent(Indent);
    OS << renderDebugBreak(Opts.TheArch);
    return true;
  }
  if (IID == llvm::Intrinsic::trap) {
    emitIndent(Indent);
    OS << "__builtin_trap();\n";
    return true;
  }
  if (IID == llvm::Intrinsic::prefetch) {
    emitIndent(Indent);
    OS << "__builtin_prefetch((void*)" << valueStr(Call.getArgOperand(0))
       << ");\n";
    return true;
  }
  if (IID == llvm::Intrinsic::aarch64_clrex) {
    emitIndent(Indent);
    OS << "__builtin_arm_clrex();\n";
    HasCIntrinsics = true;
    return true;
  }
  return false;
}

bool LLVMCWriter::writeInlineAsmCall(llvm::CallInst &Call,
                                     const std::string &Name, int Indent) {
  auto *IA = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!IA)
    return false;

  std::string AsmStr = IA->getAsmString().str();
  if (AsmStr.empty())
    return false;

  std::vector<std::string> ArgStrs;
  for (unsigned I = 0; I < Call.arg_size(); ++I)
    ArgStrs.push_back(valueStr(Call.getArgOperand(I)));

  bool ResultLive = !Call.getType()->isVoidTy();
  if (ResultLive && InferredVoid)
    ResultLive = isCallResultLive(Analysis, &Call);

  auto Render =
      renderInlineAsm(Opts.TheArch, AsmStr, Call.getType()->isStructTy(), Name,
                      ResultLive, ArgStrs);

  emitIndent(Indent);
  OS << Render.Code;
  if (Render.SetIntrinsics)
    HasCIntrinsics = true;
  return true;
}

void LLVMCWriter::writeCall(llvm::CallInst &Call, const std::string &Name,
                            int Indent) {
  if (writeIntrinsicCall(Call, Indent))
    return;
  if (writeInlineAsmCall(Call, Name, Indent))
    return;

  std::string CalleeName;
  if (auto *Callee = Call.getCalledFunction()) {
    CalleeName = Callee->getName().str();
    const char *CN = llvmIntrinsicToCName(CalleeName.c_str());
    if (CN) {
      CalleeName = CN;
    } else {
      if (!CalleeName.empty() && CalleeName[0] == '_')
        CalleeName = CalleeName.substr(1);
      for (char &Ch : CalleeName)
        if (Ch == '.')
          Ch = '_';
    }
  } else {
    CalleeName = "(" + valueStr(Call.getCalledOperand()) + ")";
  }

  emitIndent(Indent);
  bool ResultLive = !Call.getType()->isVoidTy();
  if (ResultLive && InferredVoid)
    ResultLive = isCallResultLive(Analysis, &Call);
  if (ResultLive)
    OS << Name << " = ";
  OS << CalleeName << "(";
  for (unsigned ArgIdx = 0; ArgIdx < Call.arg_size(); ++ArgIdx) {
    if (ArgIdx > 0)
      OS << ", ";
    OS << valueStr(Call.getArgOperand(ArgIdx));
  }
  OS << ");\n";
}

void LLVMCWriter::writeReturn(llvm::ReturnInst &Ret, int Indent) {
  emitIndent(Indent);
  if (InferredVoid) {
    OS << "return;\n";
    return;
  }
  if (auto *RV = Ret.getReturnValue()) {
    if (auto *Collapsed = tryCollapseHiLo(Analysis, RV)) {
      OS << "return " << valueStr(Collapsed) << ";\n";
    } else {
      auto *FnRetTy = Ret.getFunction()->getReturnType();
      std::string RetExpr = valueStr(RV);
      if (RV->getType() != FnRetTy && FnRetTy->isIntegerTy() &&
          RV->getType()->isIntegerTy() &&
          FnRetTy->getIntegerBitWidth() < RV->getType()->getIntegerBitWidth())
        RetExpr = "(" + typeToCLLVM(FnRetTy) + ")" + RetExpr;
      OS << "return " << RetExpr << ";\n";
    }
  } else {
    OS << "return;\n";
  }
}

} // namespace neverd
