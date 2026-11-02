//===- X86APXAtomic.cpp - APX atomic C projection -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate owned native calls/assembly before printing executable C.
//===----------------------------------------------------------------------===//

#include "X86APXAtomic.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace neverd {
namespace {
std::string expression(const X86APXAtomicPlan &P,
                       const std::vector<std::string> &Args) {
  if (Args.size() != P.arguments())
    throw std::invalid_argument("invalid APX atomic C argument count");
  const auto Word = "uint" + std::to_string(P.Bytes * 8) + "_t";
  std::string Temporary = "neverd_apx_atomic";
  while (std::any_of(Args.begin(), Args.end(), [&](const auto &Arg) {
    return Arg.find(Temporary) != std::string::npos;
  }))
    Temporary += "_";
  const auto Address = "(uint" + std::to_string(P.addressBytes() * 8) +
                       "_t)(uintptr_t)(" + Args[0] + ")";
  std::string Native = P.builtin() + "(" + Address;
  for (unsigned I = 1; I < Args.size(); ++I)
    Native += ", (" + Word + ")(" + Args[I] + ")";
  if (P.compare())
    Native += ", " + std::to_string(P.Condition);
  Native += ", " + std::to_string(P.AddressBits) + ", " +
            std::to_string(P.SegmentPrefix) + ")";
  const auto Offset = std::string(P.AddressBits == 16   ? "%w"
                                  : P.AddressBits == 32 ? "%k"
                                                        : "%") +
                      (P.compare() ? "1" : "0");
  const auto Segment = P.segment().empty() ? "" : "%%" + P.segment() + ":";
  std::string Fallback = "({ ";
  if (P.compare())
    Fallback += Word + " " + Temporary + " = (" + Word + ")(" + Args[1] + "); ";
  // GNU C uses escaped braces for literal assembler encoding controls.
  const auto Encoding = !P.legacy()                 ? "%{evex%} "
                        : P.compare()               ? "%{vex%} "
                        : P.TargetArch == Arch::X64 ? "%{rex%} "
                                                    : "";
  const std::string Register = P.legacy() ? "R" : "r";
  const auto Memory =
      P.stackReference() ? std::string("(%%rbp,%%rdx)") : "(" + Offset + ")";
  const auto Setup = P.stackReference()
                         ? std::string("movq %q") + (P.compare() ? "1" : "0") +
                               ", %%rdx\\n\\tsubq %%rbp, %%rdx\\n\\t"
                         : "";
  Fallback += "__asm__ __volatile__(\"" + Setup + Encoding + P.mnemonic() +
              " " + (P.compare() ? "%2, %0, " : "%1, ") + Segment + Memory +
              "\" : ";
  if (P.compare())
    Fallback += "\"+" + Register + "\"(" + Temporary + ")";
  Fallback += std::string(" : \"") +
              x86MemoryAddressAsm(P.AddressBits).GNUConstraint + "\"(" +
              Address + "), \"" + Register + "\"((" + Word + ")(" +
              Args.back() +
              ")) : " + (P.compare() || P.stackReference() ? "\"cc\", " : "") +
              (P.stackReference() ? "\"rdx\", " : "") + "\"memory\"); ";
  Fallback += P.compare() ? Temporary + "; })" : "(void)0; })";
  return std::string(P.legacy() ? "NEVERD_X86_LEGACY_ATOMIC"
                                : "NEVERD_X86_APX_ATOMIC") +
         (P.stackReference() ? "_STACK(" : "(") + Native + ", " + Fallback +
         ")";
}

bool scalar(const ExprPtr &E) {
  return E && E->Type &&
         (E->Type->Kind == NdTypeKind::Int || E->Type->Kind == NdTypeKind::Ptr);
}
} // namespace

std::optional<X86APXAtomicPlan>
x86APXAtomicCallPlan(Arch A, const llvm::CallInst &Call) {
  const auto *F = Call.getCalledFunction();
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  std::optional<X86APXAtomicPlan> P;
  if (F)
    P = x86APXAtomicName(F->getName().str());
  if (P) {
    const auto IID = llvm::Intrinsic::lookupIntrinsicID(F->getName());
    if (!IID || !F->isDeclaration() || F->getIntrinsicID() != IID ||
        F->getFunctionType() !=
            llvm::Intrinsic::getType(Call.getContext(), IID) ||
        Call.getFunctionType() != F->getFunctionType())
      throw std::invalid_argument("invalid APX native declaration");
    if (Call.arg_size() != (P->compare() ? 6 : 4))
      throw std::invalid_argument("invalid APX native argument count");
    const auto Control = [&](unsigned Index) {
      const auto *Value =
          llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(Index));
      if (!Value || !Value->getType()->isIntegerTy(32))
        throw std::invalid_argument(
            "APX address/condition controls must be i32 constants");
      return Value->getZExtValue();
    };
    const unsigned First = P->compare() ? 4 : 2;
    const auto Bits = Control(First), Prefix = Control(First + 1);
    const auto Condition = P->compare() ? Control(3) : 0;
    if (Bits > UINT32_MAX || Prefix > UINT32_MAX || Condition > UINT32_MAX)
      throw std::invalid_argument(
          "APX controls exceed their architectural width");
    P = x86APXAtomicPlan(P->Id, P->Bytes, Bits, Prefix, Condition,
                         P->TargetArch);
    if (!P)
      throw std::invalid_argument("invalid APX address/condition controls");
  } else if (Asm && llvm::StringRef(Asm->getAsmString())
                        .starts_with(X86APXAtomicAsmMarker)) {
    std::istringstream Stream(llvm::StringRef(Asm->getAsmString())
                                  .substr(X86APXAtomicAsmMarker.size())
                                  .str());
    std::string Name;
    unsigned Bits, Prefix, Condition;
    if (!(Stream >> Name >> Bits >> Prefix >> Condition))
      throw std::invalid_argument("invalid APX atomic assembly marker");
    const auto Named = x86APXAtomicName("llvm.x86." + Name + ".address" +
                                        (A == Arch::X86 ? "32" : ""));
    if (Named)
      P = x86APXAtomicPlan(Named->Id, Named->Bytes, Bits, Prefix, Condition, A);
    if (!P || Asm->getAsmString() != P->assembly() ||
        (Asm->getConstraintString() != P->constraints() &&
         (P->stackReference() ||
          Asm->getConstraintString() != P->constraints(true))) ||
        !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
        Call.arg_size() != P->arguments() ||
        Call.getFunctionType() != Asm->getFunctionType())
      throw std::invalid_argument("invalid owned APX atomic assembly");
  } else
    return std::nullopt;

  if (A != P->TargetArch || Call.getFunctionType()->isVarArg() ||
      Call.getNumOperandBundles() || Call.doesNotReturn() ||
      Call.hasFnAttr(llvm::Attribute::NoSync) ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      (P->compare() ? !Call.getType()->isIntegerTy(P->Bytes * 8)
                    : !Call.getType()->isVoidTy()))
    throw std::invalid_argument("invalid APX atomic type or effects");
  for (unsigned I = 0; I < Call.arg_size(); ++I) {
    const unsigned Bits = I < P->arguments() ? P->argumentBytes(I) * 8 : 32;
    if (!Call.getArgOperand(I)->getType()->isIntegerTy(Bits) ||
        Call.getFunctionType()->getParamType(I) !=
            Call.getArgOperand(I)->getType())
      throw std::invalid_argument("invalid APX atomic operand width");
  }
  return P;
}

std::string
renderX86APXAtomicExpression(Arch A, const HighExpr &E,
                             const std::vector<std::string> &Arguments) {
  if (E.Operands.size() > 5 || E.Kind != ExprKind::Call || E.IsIndirectCall ||
      E.IndirectTarget || E.SourceCallHint || E.DoesNotReturn ||
      !E.IntrinsicOutputs.empty() || Arguments.size() != E.Operands.size())
    throw std::invalid_argument("invalid HighC APX atomic call");
  ApxAtomicIntrinsicShape S;
  S.TargetArch = A;
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.NumInputs = E.Operands.size() + 1;
  S.IntrinsicIdIsConst = true;
  S.IntrinsicIdSize = 2;
  S.OutputIsWritableScalar = E.Type && (E.Type->Kind == NdTypeKind::Int ||
                                        E.Type->Kind == NdTypeKind::Ptr);
  S.OutputSize = E.Type ? E.Type->Size : 0;
  if (!E.Operands.empty()) {
    S.AddressIsScalar = scalar(E.Operands[0]);
    S.AddressSize = S.AddressIsScalar ? E.Operands[0]->Type->Size : 0;
  }
  if (E.Operands.size() > 1) {
    S.SourceIsScalar = scalar(E.Operands[1]);
    S.SourceSize = S.SourceIsScalar ? E.Operands[1]->Type->Size : 0;
  }
  if (E.Operands.size() > 2) {
    S.CompareIsScalar = scalar(E.Operands[2]);
    S.CompareSize = S.CompareIsScalar ? E.Operands[2]->Type->Size : 0;
  }
  if (E.Operands.size() > 3 && scalar(E.Operands[3])) {
    const auto &CC = E.Operands[3];
    S.ConditionIsConst = CC->Kind == ExprKind::Const &&
                         !isAddressProvenance(CC->ConstProvenance);
    S.ConditionSize = CC->Type->Size;
    S.Condition = CC->ConstVal;
  }
  const auto ControlIndex = x86AtomicAddressControlIndex(E.IntrinsicId) - 1;
  if (E.Operands.size() > ControlIndex && scalar(E.Operands[ControlIndex])) {
    const auto &Control = E.Operands[ControlIndex];
    S.AddressControlIsConst = Control->Kind == ExprKind::Const &&
                              !isAddressProvenance(Control->ConstProvenance);
    S.AddressControlSize = Control->Type->Size;
    S.AddressControl = Control->ConstVal;
  }
  const auto Prefix = x86APXAtomicNativePrefix(S);
  if (!intrinsicApxAtomicShapeIsValid(E.IntrinsicId, S) || !Prefix)
    throw std::invalid_argument("invalid HighC APX atomic operands");
  const auto Plan = x86APXAtomicPlan(
      E.IntrinsicId, S.SourceSize, A == Arch::X86 ? 32 : 64, *Prefix,
      isX86CmpccXaddIntrinsic(E.IntrinsicId) ? S.Condition : 0, A);
  if (!Plan)
    throw std::invalid_argument("invalid HighC APX atomic target");
  const auto &P = *Plan;
  std::vector<std::string> Args = {Arguments[0]};
  if (P.compare())
    Args.push_back(Arguments[2]);
  Args.push_back(Arguments[1]);
  return expression(P, Args);
}

std::optional<std::string>
renderX86APXAtomicCall(Arch A, const llvm::CallInst &Call,
                       const std::string &Result, bool ResultLive,
                       const std::vector<std::string> &Arguments) {
  const auto P = x86APXAtomicCallPlan(A, Call);
  if (!P)
    return std::nullopt;
  if (Arguments.size() != Call.arg_size() ||
      (ResultLive && (Result.empty() || !P->compare())))
    throw std::invalid_argument("invalid APX atomic result binding");
  return (ResultLive ? Result + " = " : "") +
         expression(*P,
                    {Arguments.begin(), Arguments.begin() + P->arguments()}) +
         ";\n";
}
} // namespace neverd
