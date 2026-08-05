//===- LLVMCExprWriter.cpp - LLVM IR value/expression rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Value and expression rendering for the LLVM IR C emitter: converts LLVM
/// Values, Constants, and inline-able instructions to C source strings.
///
//===----------------------------------------------------------------------===//

#include "LLVMCWriter.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cctype>

namespace neverd {

std::string LLVMCWriter::resolveNdDataName(llvm::StringRef Name) const {
  llvm::StringRef Hex;
  if (Name.starts_with(kNdDataPrefix))
    Hex = Name.drop_front(kNdDataPrefix.size());
  else if (Name.starts_with("_") &&
           Name.drop_front(1).starts_with(kNdDataPrefix))
    Hex = Name.drop_front(1 + kNdDataPrefix.size());
  if (Hex.empty())
    return {};

  uint64_t Addr;
  if (Hex.getAsInteger(16, Addr))
    return {};

  if (Img) {
    if (const auto *Sym = Img->findSymbolAt(Addr))
      if (!Sym->Name.empty())
        return Sym->Name;
  }

  return "data_" + llvm::utohexstr(Addr);
}

std::string LLVMCWriter::freshVar(const std::string &Hint) {
  std::string Name;
  do {
    Name = Hint + std::to_string(NextVar++);
  } while (UsedNames.count(Name));
  UsedNames.insert(Name);
  return Name;
}

std::string LLVMCWriter::getName(const llvm::Value *V) {
  auto It = ValNames.find(V);
  if (It != ValNames.end())
    return It->second;

  std::string Hint = "v";
  if (V->hasName()) {
    std::string Raw = V->getName().str();
    std::string Clean;
    for (char Ch : Raw) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Clean += Ch;
      else
        Clean += '_';
    }
    if (!Clean.empty() &&
        !std::isdigit(static_cast<unsigned char>(Clean[0])))
      Hint = Clean;
  }
  auto Name = freshVar(Hint);
  ValNames[V] = Name;
  return Name;
}

std::string LLVMCWriter::constStr(const llvm::Constant *C) {
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(C)) {
    if (CI->getType()->isIntegerTy(1))
      return CI->isZero() ? "0" : "1";
    if (CI->isNegative() && CI->getBitWidth() <= 64)
      return std::to_string(CI->getSExtValue());
    return std::to_string(CI->getZExtValue());
  }

  if (auto *CF = llvm::dyn_cast<llvm::ConstantFP>(C)) {
    llvm::SmallString<32> Buf;
    CF->getValueAPF().toString(Buf, 0, 0);
    auto S = std::string(Buf.str());
    if (C->getType()->isFloatTy())
      S += "f";
    return S;
  }

  if (llvm::isa<llvm::ConstantPointerNull>(C))
    return "((void*)0)";

  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(C)) {
    if (CE->getOpcode() == llvm::Instruction::GetElementPtr) {
      if (CE->getNumOperands() >= 3) {
        if (auto *GV =
                llvm::dyn_cast<llvm::GlobalVariable>(CE->getOperand(0))) {
          if (GV->hasInitializer()) {
            if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataArray>(
                    GV->getInitializer())) {
              if (CDA->isString())
                return "\"" + escapeCString(CDA->getAsString()) + "\"";
            }
          }
        }
      }
    }
    if (CE->getOpcode() == llvm::Instruction::PtrToInt) {
      auto *Src = CE->getOperand(0);
      if (auto *Fn = llvm::dyn_cast<llvm::Function>(Src)) {
        std::string N = Fn->getName().str();
        if (!N.empty() && N[0] == '_')
          N = N.substr(1);
        return "(" + typeToCLLVM(CE->getType()) + ")(void*)" + N;
      }
      if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Src)) {
        if (GV->hasInitializer()) {
          if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataArray>(
                  GV->getInitializer())) {
            llvm::StringRef Raw = CDA->getRawDataValues();
            uint64_t Val = 0;
            size_t Len = std::min<size_t>(Raw.size(), 8);
            for (size_t I = 0; I < Len; ++I)
              Val |= static_cast<uint64_t>(static_cast<uint8_t>(Raw[I]))
                     << (I * 8);
            return "0x" + llvm::utohexstr(Val);
          }
        }
      }
      return "(" + typeToCLLVM(CE->getType()) + ")(void*)" + valueStr(Src);
    }
    if (CE->getOpcode() == llvm::Instruction::BitCast ||
        CE->getOpcode() == llvm::Instruction::IntToPtr) {
      return "(" + typeToCLLVM(CE->getType()) + ")" +
             valueStr(CE->getOperand(0));
    }
  }

  if (auto *GV = llvm::dyn_cast<llvm::GlobalValue>(C)) {
    std::string N = GV->getName().str();
    if (N.empty())
      return getName(C);

    std::string Resolved = resolveNdDataName(N);
    if (!Resolved.empty())
      return Resolved;

    if (N[0] == '_')
      N = N.substr(1);
    std::string Clean;
    for (char Ch : N) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Clean += Ch;
      else
        Clean += '_';
    }
    if (Clean.empty() ||
        std::isdigit(static_cast<unsigned char>(Clean[0])))
      Clean = "g_" + Clean;
    return Clean;
  }

  if (llvm::isa<llvm::UndefValue>(C))
    return "0";

  if (auto *CA = llvm::dyn_cast<llvm::ConstantAggregate>(C)) {
    std::string S = "{";
    for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
      if (I > 0)
        S += ", ";
      S += constStr(llvm::cast<llvm::Constant>(CA->getOperand(I)));
    }
    S += "}";
    return S;
  }

  if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataSequential>(C)) {
    if (CDA->isString())
      return "\"" + escapeCString(CDA->getAsString()) + "\"";
    std::string S = "{";
    for (unsigned I = 0; I < CDA->getNumElements(); ++I) {
      if (I > 0)
        S += ", ";
      S += constStr(CDA->getElementAsConstant(I));
    }
    S += "}";
    return S;
  }

  if (llvm::isa<llvm::ConstantAggregateZero>(C))
    return "{0}";

  return "0";
}

std::string LLVMCWriter::valueStr(const llvm::Value *V) {
  auto Fwd = Analysis.ForwardedLoads.find(V);
  if (Fwd != Analysis.ForwardedLoads.end())
    return valueStr(Fwd->second);
  if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(V)) {
    auto It = Analysis.IntrinsicStructNames.find(EV->getAggregateOperand());
    if (It != Analysis.IntrinsicStructNames.end())
      return It->second + "[" + std::to_string(EV->getIndices()[0]) + "]";
  }
  if (Analysis.Inlinable.count(V)) {
    auto It = InlineCache.find(V);
    if (It != InlineCache.end())
      return It->second;
    auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
    if (Inst) {
      auto Expr = renderInline(*Inst);
      InlineCache[V] = Expr;
      return Expr;
    }
  }
  if (auto *C = llvm::dyn_cast<llvm::Constant>(V))
    return constStr(C);
  return getName(V);
}

std::string LLVMCWriter::blockLabel(const llvm::BasicBlock *BB) {
  auto It = BlockLabels.find(BB);
  if (It != BlockLabels.end())
    return It->second;
  std::string Label;
  if (BB->hasName()) {
    std::string Raw = BB->getName().str();
    Label = "L_";
    for (char Ch : Raw) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Label += Ch;
      else
        Label += '_';
    }
  } else {
    Label = "L_" + std::to_string(BlockLabels.size());
  }
  BlockLabels[BB] = Label;
  return Label;
}

std::string LLVMCWriter::binopStr(unsigned Opcode, const std::string &LHS,
                                  const std::string &RHS, llvm::Type * /*Ty*/) {
  switch (Opcode) {
  case llvm::Instruction::Add:
  case llvm::Instruction::FAdd:
    return LHS + " + " + RHS;
  case llvm::Instruction::Sub:
  case llvm::Instruction::FSub:
    return LHS + " - " + RHS;
  case llvm::Instruction::Mul:
  case llvm::Instruction::FMul:
    return LHS + " * " + RHS;
  case llvm::Instruction::UDiv:
    return "(unsigned)" + LHS + " / (unsigned)" + RHS;
  case llvm::Instruction::SDiv:
  case llvm::Instruction::FDiv:
    return LHS + " / " + RHS;
  case llvm::Instruction::URem:
    return "(unsigned)" + LHS + " % (unsigned)" + RHS;
  case llvm::Instruction::SRem:
  case llvm::Instruction::FRem:
    return LHS + " % " + RHS;
  case llvm::Instruction::Shl:
    return LHS + " << " + RHS;
  case llvm::Instruction::LShr:
    return "(unsigned)" + LHS + " >> " + RHS;
  case llvm::Instruction::AShr:
    return LHS + " >> " + RHS;
  case llvm::Instruction::And:
    return LHS + " & " + RHS;
  case llvm::Instruction::Or:
    return LHS + " | " + RHS;
  case llvm::Instruction::Xor:
    return LHS + " ^ " + RHS;
  default:
    return LHS + " /* unknown_binop */ " + RHS;
  }
}

std::string LLVMCWriter::castStr(unsigned /*Opcode*/, const std::string &Src,
                                 llvm::Type * /*SrcTy*/, llvm::Type *DstTy) {
  std::string Dst = typeToCLLVM(DstTy);
  return "(" + Dst + ")" + Src;
}

std::string LLVMCWriter::cmpStr(llvm::CmpInst::Predicate Pred,
                                const std::string &LHS, const std::string &RHS,
                                bool /*IsFP*/) {
  switch (Pred) {
  case llvm::CmpInst::ICMP_EQ:
  case llvm::CmpInst::FCMP_OEQ:
  case llvm::CmpInst::FCMP_UEQ:
    return LHS + " == " + RHS;
  case llvm::CmpInst::ICMP_NE:
  case llvm::CmpInst::FCMP_ONE:
  case llvm::CmpInst::FCMP_UNE:
    return LHS + " != " + RHS;
  case llvm::CmpInst::ICMP_UGT:
    return "(unsigned)" + LHS + " > (unsigned)" + RHS;
  case llvm::CmpInst::ICMP_UGE:
    return "(unsigned)" + LHS + " >= (unsigned)" + RHS;
  case llvm::CmpInst::ICMP_ULT:
    return "(unsigned)" + LHS + " < (unsigned)" + RHS;
  case llvm::CmpInst::ICMP_ULE:
    return "(unsigned)" + LHS + " <= (unsigned)" + RHS;
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::FCMP_OGT:
  case llvm::CmpInst::FCMP_UGT:
    return LHS + " > " + RHS;
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::FCMP_OGE:
  case llvm::CmpInst::FCMP_UGE:
    return LHS + " >= " + RHS;
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::FCMP_OLT:
  case llvm::CmpInst::FCMP_ULT:
    return LHS + " < " + RHS;
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::FCMP_OLE:
  case llvm::CmpInst::FCMP_ULE:
    return LHS + " <= " + RHS;
  case llvm::CmpInst::FCMP_ORD:
    return LHS + " == " + LHS + " && " + RHS + " == " + RHS;
  case llvm::CmpInst::FCMP_UNO:
    return LHS + " != " + LHS + " || " + RHS + " != " + RHS;
  default:
    return LHS + " /* unknown_cmp */ " + RHS;
  }
}

std::string LLVMCWriter::renderInline(const llvm::Instruction &Inst) {
  if (Inst.isBinaryOp()) {
    auto LHS = valueStr(Inst.getOperand(0));
    auto RHS = valueStr(Inst.getOperand(1));
    return "(" + binopStr(Inst.getOpcode(), LHS, RHS, Inst.getType()) + ")";
  }
  if (Inst.isCast()) {
    auto Src = valueStr(Inst.getOperand(0));
    return castStr(Inst.getOpcode(), Src, Inst.getOperand(0)->getType(),
                   Inst.getType());
  }
  if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&Inst)) {
    auto Base = valueStr(GEP->getPointerOperand());
    if (GEP->getNumIndices() == 1)
      return "(void*)((char*)" + Base + " + " + valueStr(GEP->getOperand(1)) +
             ")";
    if (GEP->getNumIndices() == 2) {
      auto Idx0 = valueStr(GEP->getOperand(1));
      auto Idx1 = valueStr(GEP->getOperand(2));
      if (Idx0 == "0") {
        if (auto *AT =
                llvm::dyn_cast<llvm::ArrayType>(GEP->getSourceElementType()))
          return "&((" + typeToCLLVM(AT->getElementType()) + "*)" + Base +
                 ")[" + Idx1 + "]";
      }
      return "(void*)((char*)" + Base + " + " + Idx1 + ")";
    }
    return "(void*)" + Base;
  }
  if (auto *CI = llvm::dyn_cast<llvm::ICmpInst>(&Inst)) {
    auto LHS = valueStr(CI->getOperand(0));
    auto RHS = valueStr(CI->getOperand(1));
    return "(" + cmpStr(CI->getPredicate(), LHS, RHS, false) + ")";
  }
  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&Inst)) {
    return "(" + valueStr(Sel->getCondition()) + " ? " +
           valueStr(Sel->getTrueValue()) + " : " +
           valueStr(Sel->getFalseValue()) + ")";
  }
  return getName(&Inst);
}

} // namespace neverd
