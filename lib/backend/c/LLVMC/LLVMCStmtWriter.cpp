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

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"

#include <cctype>

namespace neverd {

namespace {
constexpr uint32_t kCxxCatchConst = 0x1u;
constexpr uint32_t kCxxCatchVolatile = 0x2u;
constexpr uint32_t kCxxCatchReference = 0x8u;
constexpr uint32_t kCxxCatchAll = 0x40u;

bool isCIdentifier(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  return llvm::all_of(Name, [](char Ch) {
    return std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_';
  });
}

std::string cxxTypeFromDescriptor(const BinaryImage *Img, va_t DescriptorVA) {
  if (!Img || !DescriptorVA)
    return {};
  const size_t PointerSize = Img->is64Bit() ? 8 : 4;
  if (DescriptorVA > InvalidVA - 2 * PointerSize)
    return {};
  const va_t NameVA = DescriptorVA + 2 * PointerSize;
  std::string Name;
  for (size_t I = 0; I < limits::kMaxMsvcTypeDescriptorNameBytes; ++I) {
    if (I > InvalidVA - NameVA)
      return {};
    const uint8_t *Byte = Img->readVA(NameVA + I, 1);
    if (!Byte)
      return {};
    if (*Byte == 0)
      break;
    Name.push_back(static_cast<char>(*Byte));
  }
  llvm::StringRef Mangled(Name);
  if (Mangled.starts_with(".?A") && Mangled.size() > 4) {
    llvm::StringRef Rest = Mangled.drop_front(4);
    const size_t At = Rest.find('@');
    if (At != llvm::StringRef::npos)
      Rest = Rest.take_front(At);
    if (isCIdentifier(Rest))
      return Rest.str();
  }
  return Name;
}
} // namespace

void LLVMCWriter::emitIndent(int N) { emitCIndent(OS, N); }

const llvm::AllocaInst *
LLVMCWriter::asAllocaPointer(const llvm::Value *V) const {
  if (!V)
    return nullptr;
  return llvm::dyn_cast<llvm::AllocaInst>(V->stripPointerCasts());
}

void LLVMCWriter::writeInstruction(llvm::Instruction &Inst, int Indent) {
  if (llvm::isa<llvm::DbgInfoIntrinsic>(&Inst))
    return;
  if (Analysis.Inlinable.count(&Inst))
    return;
  if (Analysis.DeadFrameStores.count(&Inst))
    return;
  if (AfterCxxThrow && (llvm::isa<llvm::ReturnInst>(&Inst) ||
                        llvm::isa<llvm::UnreachableInst>(&Inst)))
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
    if (isImportCalleeOnlyLoad(LI))
      return;
    emitIndent(Indent);
    if (std::string Seg = renderX86SegmentedLoad(
            Opts.TheArch, *LI,
            [this](const llvm::Value *V) { return valueStr(V); });
        !Seg.empty()) {
      OS << Name << " = " << Seg << ";\n";
      HasCIntrinsics = true;
      return;
    }
    if (const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand()))
      OS << Name << " = " << getName(Slot) << ";\n";
    else if (std::string Image = imageDataCName(LI->getPointerOperand());
             !Image.empty())
      OS << Name << " = " << Image << ";\n";
    else
      OS << Name << " = *(" << typeToCLLVM(LI->getType()) << "*)"
         << valueStr(LI->getPointerOperand()) << ";\n";
    return;
  }

  if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
    emitIndent(Indent);
    if (const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand()))
      OS << getName(Slot) << " = " << valueStr(SI->getValueOperand()) << ";\n";
    else if (std::string Image = imageDataCName(SI->getPointerOperand());
             !Image.empty())
      OS << Image << " = " << valueStr(SI->getValueOperand()) << ";\n";
    else
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

  if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Inst)) {
    writeInvoke(*Invoke, Name, Indent);
    return;
  }

  if (auto *CS = llvm::dyn_cast<llvm::CatchSwitchInst>(&Inst)) {
    writeCatchSwitch(*CS, Indent);
    return;
  }

  if (llvm::isa<llvm::CatchPadInst, llvm::CleanupPadInst>(&Inst))
    return;

  if (auto *CR = llvm::dyn_cast<llvm::CatchReturnInst>(&Inst)) {
    writePhiCopies(Inst.getParent(), CR->getSuccessor(), Indent);
    emitIndent(Indent);
    OS << "goto " << blockLabel(CR->getSuccessor()) << ";\n";
    return;
  }

  if (auto *Clean = llvm::dyn_cast<llvm::CleanupReturnInst>(&Inst)) {
    writeCleanupRet(*Clean, Indent);
    return;
  }

  if (auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(&Inst)) {
    const llvm::BasicBlock *From = Inst.getParent();
    writePhiCopies(From, Br->getSuccessor(0), Indent);
    emitIndent(Indent);
    OS << "goto " << blockLabel(Br->getSuccessor(0)) << ";\n";
    return;
  }

  if (auto *Br = llvm::dyn_cast<llvm::CondBrInst>(&Inst)) {
    const llvm::BasicBlock *From = Inst.getParent();
    emitIndent(Indent);
    OS << "if (" << valueStr(Br->getCondition()) << ") {\n";
    writePhiCopies(From, Br->getSuccessor(0), Indent + 1);
    emitIndent(Indent + 1);
    OS << "goto " << blockLabel(Br->getSuccessor(0)) << ";\n";
    emitIndent(Indent);
    OS << "} else {\n";
    writePhiCopies(From, Br->getSuccessor(1), Indent + 1);
    emitIndent(Indent + 1);
    OS << "goto " << blockLabel(Br->getSuccessor(1)) << ";\n";
    emitIndent(Indent);
    OS << "}\n";
    return;
  }

  if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&Inst)) {
    writeReturn(*Ret, Indent);
    return;
  }

  if (auto *SW = llvm::dyn_cast<llvm::SwitchInst>(&Inst)) {
    const llvm::BasicBlock *From = Inst.getParent();
    emitIndent(Indent);
    OS << "switch (" << valueStr(SW->getCondition()) << ") {\n";
    for (auto &C : SW->cases()) {
      emitIndent(Indent);
      OS << "case " << C.getCaseValue()->getSExtValue() << ": {\n";
      writePhiCopies(From, C.getCaseSuccessor(), Indent + 1);
      emitIndent(Indent + 1);
      OS << "goto " << blockLabel(C.getCaseSuccessor()) << ";\n";
      emitIndent(Indent);
      OS << "}\n";
    }
    emitIndent(Indent);
    OS << "default: {\n";
    writePhiCopies(From, SW->getDefaultDest(), Indent + 1);
    emitIndent(Indent + 1);
    OS << "goto " << blockLabel(SW->getDefaultDest()) << ";\n";
    emitIndent(Indent);
    OS << "}\n";
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

  if (llvm::isa<llvm::PHINode>(&Inst))
    return;

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

  if (auto *FI = llvm::dyn_cast<llvm::FreezeInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = " << valueStr(FI->getOperand(0)) << ";\n";
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
  if (const llvm::AllocaInst *Slot = asAllocaPointer(GEP.getPointerOperand())) {
    const std::string Local = getName(Slot);
    if (GEP.getNumIndices() == 1) {
      const std::string Idx = valueStr(GEP.getOperand(1));
      if (Idx == "0")
        OS << Name << " = &" << Local << ";\n";
      else
        OS << Name << " = (void*)((char*)&" << Local << " + " << Idx << ");\n";
      return;
    }
    if (GEP.getNumIndices() == 2 && valueStr(GEP.getOperand(1)) == "0") {
      OS << Name << " = &" << Local << "[" << valueStr(GEP.getOperand(2))
         << "];\n";
      return;
    }
  }
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

bool LLVMCWriter::writeIntrinsicCall(llvm::CallBase &Call, int Indent) {
  auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(&Call);
  if (!II)
    return false;

  auto IID = II->getIntrinsicID();
  if (IID == llvm::Intrinsic::sideeffect || IID == llvm::Intrinsic::donothing ||
      IID == llvm::Intrinsic::seh_try_begin ||
      IID == llvm::Intrinsic::seh_try_end ||
      IID == llvm::Intrinsic::localaddress ||
      IID == llvm::Intrinsic::localescape ||
      IID == llvm::Intrinsic::localrecover)
    return true;
  if (IID == llvm::Intrinsic::eh_exceptioncode) {
    if (!Call.getType()->isVoidTy()) {
      emitIndent(Indent);
      OS << getName(&Call) << " = GetExceptionCode();\n";
    }
    return true;
  }
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
    if (AfterCxxThrow)
      return true;
    emitIndent(Indent);
    OS << renderDebugBreak(Opts.TheArch);
    AfterCxxThrow = false;
    return true;
  }
  if (IID == llvm::Intrinsic::trap) {
    if (AfterCxxThrow)
      return true;
    emitIndent(Indent);
    OS << "__builtin_trap();\n";
    AfterCxxThrow = false;
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

void LLVMCWriter::writeCallLike(llvm::CallBase &Call, const std::string &Name,
                                int Indent) {
  if (writeIntrinsicCall(Call, Indent))
    return;
  if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Call))
    if (writeInlineAsmCall(*CI, Name, Indent))
      return;

  std::string CalleeName;
  if (auto *Callee = Call.getCalledFunction()) {
    const std::string RawCalleeName = Callee->getName().str();
    const char *CN = llvmIntrinsicToCName(RawCalleeName.c_str());
    if (CN) {
      CalleeName = CN;
    } else {
      CalleeName = functionIdentifier(*Callee);
    }
  } else {
    CalleeName = resolveImportCalleeName(Call.getCalledOperand());
    if (CalleeName.empty())
      CalleeName = "(" + valueStr(Call.getCalledOperand()) + ")";
  }

  if (isMsvcCxxThrowCallName(CalleeName) ||
      (Call.getCalledFunction() &&
       isMsvcCxxThrowCallName(Call.getCalledFunction()->getName()))) {
    emitIndent(Indent);
    OS << "throw";
    if (Call.arg_size() > 0)
      OS << " " << valueStr(Call.getArgOperand(0));
    OS << ";\n";
    AfterCxxThrow = true;
    return;
  }

  const bool NoReturn = callDoesNotReturn(Call);
  if (Call.getCalledFunction() &&
      isX86FastFailName(Call.getCalledFunction()->getName())) {
    CalleeName = "__fastfail";
    HasCIntrinsics = true;
  }

  emitIndent(Indent);
  bool ResultLive = !Call.getType()->isVoidTy();
  if (ResultLive && NoReturn)
    ResultLive = false;
  else if (ResultLive && InferredVoid) {
    if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Call))
      ResultLive = isCallResultLive(Analysis, CI);
  }
  if (ResultLive)
    OS << Name << " = ";
  OS << CalleeName << "(";
  for (unsigned ArgIdx = 0; ArgIdx < Call.arg_size(); ++ArgIdx) {
    if (ArgIdx > 0)
      OS << ", ";
    OS << valueStr(Call.getArgOperand(ArgIdx));
  }
  OS << ");\n";
  AfterCxxThrow = NoReturn;
}

void LLVMCWriter::writePhiCopies(const llvm::BasicBlock *From,
                                 const llvm::BasicBlock *To, int Indent) {
  if (!From || !To)
    return;
  for (const llvm::Instruction &Inst : *To) {
    const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
    if (!Phi)
      break;
    if (Analysis.Inlinable.count(Phi))
      continue;
    llvm::Value *Incoming = Phi->getIncomingValueForBlock(From);
    if (!Incoming)
      continue;
    emitIndent(Indent);
    OS << getName(Phi) << " = " << valueStr(Incoming) << ";\n";
  }
}

std::string
LLVMCWriter::resolveImportCalleeName(const llvm::Value *Callee) const {
  if (!Callee)
    return {};
  const llvm::Value *Op = Callee->stripPointerCasts();
  for (unsigned Depth = 0; Op && Depth < 4; ++Depth) {
    if (const auto *Fn = llvm::dyn_cast<llvm::Function>(Op))
      return functionIdentifier(*Fn);
    if (const auto *CI = llvm::dyn_cast<llvm::CastInst>(Op)) {
      if (CI->getOpcode() == llvm::Instruction::IntToPtr ||
          CI->getOpcode() == llvm::Instruction::PtrToInt ||
          CI->getOpcode() == llvm::Instruction::BitCast) {
        Op = CI->getOperand(0)->stripPointerCasts();
        continue;
      }
    }
    if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(Op)) {
      if (CE->isCast()) {
        Op = CE->getOperand(0)->stripPointerCasts();
        continue;
      }
    }
    break;
  }
  const llvm::Value *Ptr = Op;
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Op))
    Ptr = LI->getPointerOperand()->stripPointerCasts();
  const auto *GV = llvm::dyn_cast<llvm::GlobalValue>(Ptr);
  if (!GV)
    return {};
  const std::optional<va_t> Slot = parseNdCodePtrSymbol(GV->getName());
  if (!Slot || !Img)
    return {};
  if (const Import *Imp = Img->findImportAt(*Slot); Imp && !Imp->Name.empty())
    return Imp->Name;
  return {};
}

bool LLVMCWriter::isImportCalleeOnlyLoad(const llvm::LoadInst *LI) const {
  if (!LI || LI->use_empty())
    return false;
  for (const llvm::User *User : LI->users()) {
    const llvm::Value *Callee = User;
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(User)) {
      if (Cast->user_empty())
        return false;
      for (const llvm::User *CastUser : Cast->users()) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(CastUser);
        if (!Call || Call->getCalledOperand() != Cast ||
            resolveImportCalleeName(Call->getCalledOperand()).empty())
          return false;
      }
      continue;
    }
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
    if (!Call || Call->getCalledOperand() != LI ||
        resolveImportCalleeName(Call->getCalledOperand()).empty())
      return false;
  }
  return true;
}

bool LLVMCWriter::callDoesNotReturn(const llvm::CallBase &Call) const {
  if (Call.doesNotReturn())
    return true;
  const auto *Fn = Call.getCalledFunction();
  if (!Fn)
    return false;
  llvm::StringRef Name = Fn->getName();
  if (isX86FastFailName(Name))
    return true;
  return libc::isNoReturnFunction(Name);
}

void LLVMCWriter::writeCall(llvm::CallInst &Call, const std::string &Name,
                            int Indent) {
  writeCallLike(Call, Name, Indent);
}

void LLVMCWriter::writeInvoke(llvm::InvokeInst &Invoke, const std::string &Name,
                              int Indent) {
  writeCallLike(Invoke, Name, Indent);
}

std::string LLVMCWriter::windowsEHFilterExpr(const llvm::CatchSwitchInst &CS) {
  if (CS.getNumHandlers() == 0)
    return "EXCEPTION_EXECUTE_HANDLER";
  const llvm::BasicBlock *PadBB = *CS.handler_begin();
  const llvm::Instruction *First = nullptr;
  if (PadBB)
    for (const llvm::Instruction &Inst : *PadBB)
      if (!llvm::isa<llvm::PHINode>(&Inst)) {
        First = &Inst;
        break;
      }
  const auto *Pad = llvm::dyn_cast_or_null<llvm::CatchPadInst>(First);
  if (!Pad || Pad->arg_size() == 0)
    return "EXCEPTION_EXECUTE_HANDLER";
  llvm::Value *Filter = Pad->getArgOperand(0)->stripPointerCasts();
  if (llvm::isa<llvm::ConstantPointerNull>(Filter))
    return "EXCEPTION_EXECUTE_HANDLER";
  if (const auto *Fn = llvm::dyn_cast<llvm::Function>(Filter))
    return functionIdentifier(*Fn) + "(GetExceptionInformation())";
  return "nd_seh_filter_0x0(GetExceptionInformation())";
}

std::string LLVMCWriter::windowsCxxCatchType(const llvm::CatchPadInst &Pad) {
  uint32_t Adjectives = 0;
  if (Pad.arg_size() >= 2)
    if (const auto *CI =
            llvm::dyn_cast<llvm::ConstantInt>(Pad.getArgOperand(1)))
      Adjectives = static_cast<uint32_t>(CI->getZExtValue());
  if ((Adjectives & kCxxCatchAll) != 0 || Pad.arg_size() == 0)
    return "...";
  std::string Type = "...";
  if (Pad.arg_size() != 0) {
    llvm::Value *Desc = Pad.getArgOperand(0)->stripPointerCasts();
    if (const auto *GV = llvm::dyn_cast<llvm::GlobalValue>(Desc)) {
      if (auto VA = parseNdDataSymbol(GV->getName())) {
        std::string Recovered = cxxTypeFromDescriptor(Img, *VA);
        if (!Recovered.empty() && isCIdentifier(Recovered))
          Type = Recovered;
        else
          Type = "/* type @ 0x" + llvm::utohexstr(*VA) + " */";
      }
    }
  }
  std::string Result;
  if (Adjectives & kCxxCatchConst)
    Result += "const ";
  if (Adjectives & kCxxCatchVolatile)
    Result += "volatile ";
  Result += Type;
  if (Adjectives & kCxxCatchReference)
    Result += " &";
  return Result;
}

void LLVMCWriter::writeCatchSwitch(llvm::CatchSwitchInst &CS, int Indent) {
  emitIndent(Indent);
  const llvm::BasicBlock *Target = nullptr;
  if (CS.getNumHandlers() != 0) {
    const llvm::BasicBlock *PadBB = *CS.handler_begin();
    if (PadBB)
      for (const llvm::Instruction &Inst : *PadBB)
        if (const auto *Ret = llvm::dyn_cast<llvm::CatchReturnInst>(&Inst)) {
          Target = Ret->getSuccessor();
          break;
        }
    if (!Target)
      Target = PadBB;
  }
  const llvm::Instruction *First = nullptr;
  if (CS.getNumHandlers())
    for (const llvm::Instruction &Inst : **CS.handler_begin())
      if (!llvm::isa<llvm::PHINode>(&Inst)) {
        First = &Inst;
        break;
      }
  const auto *Pad = llvm::dyn_cast_or_null<llvm::CatchPadInst>(First);
  const bool Cxx = Pad && Pad->arg_size() >= 2;
  if (Cxx)
    OS << "/* catch (" << (Pad ? windowsCxxCatchType(*Pad) : "...") << ") */ ";
  else
    OS << "/* __except (" << windowsEHFilterExpr(CS) << ") */ ";
  if (Target)
    OS << "goto " << blockLabel(Target) << ";\n";
  else
    OS << ";\n";
}

void LLVMCWriter::writeCleanupRet(llvm::CleanupReturnInst &CR, int Indent) {
  emitIndent(Indent);
  if (llvm::BasicBlock *Dest = CR.getUnwindDest())
    OS << "goto " << blockLabel(Dest) << "; /* __finally */\n";
  else
    OS << "return; /* __finally */\n";
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
