//===- X86FourSource.cpp - Four-source C projection ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Carry the accumulator and every source through native calls or checked asm.
//===----------------------------------------------------------------------===//
#include "X86FourSource.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace neverd {
namespace {
std::string expression(const X86FourSourcePlan &P,
                       const std::vector<std::string> &Args,
                       const std::string &Type, bool Live,
                       X86FourSourceOperandUse Use) {
  // HighIR carries one packed source; LLVM calls carry all four separately.
  const bool Packed = Args.size() == 4;
  if ((!Packed && Args.size() != 7) || (Live && Type.empty()))
    throw std::invalid_argument(X86FourSourceMalformed);
  std::string T = "neverd_four";
  while (std::any_of(Args.begin(), Args.end(), [&](const auto &A) {
    return A.find(T) != std::string::npos;
  }))
    T += '_';
  const auto V = T + "_type", Acc = T + "_acc", Src = T + "_src";
  const auto Bytes = std::to_string(P.Control.bytes());
  const auto AddressValue =
      Use.Address ? "(void *)(uintptr_t)(" + Args[0] + ")" : "(void *)0";
  const auto Address = T + "_address", Mask = T + "_mask";
  std::string Text = "({ typedef unsigned int " + V +
                     " __attribute__((vector_size(" + Bytes + "))); " + V +
                     " " + Acc + " = {0}, " + Src + "[4] = {{0}}; ";
  if (Use.Accumulator)
    Text +=
        (Packed
             ? "unsigned _BitInt(" + std::to_string(P.Control.bytes() * 8) + ")"
             : "__auto_type") +
        std::string(" ") + T + "_old = (" + Args[1] + "); __builtin_memcpy(&" +
        Acc + ", &" + T + "_old, " + Bytes + "); ";
  if (Use.Sources) {
    if (Packed) {
      const auto Input = T + "_packed";
      Text += "unsigned _BitInt(" +
              std::to_string(P.Control.sourceBytes() * X86FourSourceCount * 8) +
              ") " + Input + " = (" + Args[2] + "); ";
      for (unsigned I = 0; I < X86FourSourceCount; ++I)
        Text += "__builtin_memcpy(&" + Src + "[" + std::to_string(I) +
                "], (const unsigned char *)&" + Input + " + " +
                std::to_string(I * P.Control.sourceBytes()) + ", " +
                std::to_string(P.Control.sourceBytes()) + "); ";
    } else {
      for (unsigned I = 0; I < X86FourSourceCount; ++I) {
        const auto Input = T + "_input" + std::to_string(I);
        Text += "__auto_type " + Input + " = (" + Args[2 + I] +
                "); __builtin_memcpy(&" + Src + "[" + std::to_string(I) +
                "], &" + Input + ", " + Bytes + "); ";
      }
    }
  }
  // Evaluate calls in address/mask expressions before binding the four fixed
  // registers. Such a call may overwrite every caller-saved vector register.
  Text += "void *" + Address + " = " + AddressValue + "; uint16_t " + Mask +
          " = (uint16_t)(" + Args.back() + "); ";
  std::string Native = Acc + " = " + P.builtin() + "(" + Address + ", " + Acc;
  for (unsigned I = 0; I < X86FourSourceCount; ++I)
    Native += ", " + Src + "[" + std::to_string(I) + "]";
  Native += ", (uint16_t)(" + Mask + "), " + std::to_string(P.Control.Zero) +
            ", " + std::to_string(P.Prefix) + ")";
  std::string Fallback = "({ ";
  for (unsigned I = 0; I < X86FourSourceCount; ++I)
    Fallback += "register " + V + " " + T + "_r" + std::to_string(I) +
                " __asm__(\"" + P.sourceRegister(I) + "\") = " + Src + "[" +
                std::to_string(I) + "]; ";
  const auto Segment =
      P.hasSegmentPrefix() ? std::string("%%") + P.segment() + ":" : "";
  const auto IntelSegment =
      P.hasSegmentPrefix() ? std::string(P.segment()) + ":" : "";
  const auto Suffix = std::string(" %{%2%}") + (P.Control.Zero ? " %{z%}" : "");
  const auto Setup = P.stackReference() ? "mov{q %q1, %%rdx| rdx, %q1}\\n\\t"
                                          "sub{q %%rbp, %%rdx| rdx, rbp}\\n\\t"
                                        : "";
  const auto AttMem = P.stackReference() ? "(%%rbp,%%rdx)" : "(%1)";
  const auto IntelMem = P.stackReference() ? "[rbp+rdx]" : "[%1]";
  Fallback += std::string("__asm__ __volatile__(\"") + Setup +
              P.Control.mnemonic() + " {" + Segment + AttMem + ", %3, %0" +
              Suffix + "|%0" + Suffix + ", %3, " + IntelSegment + IntelMem +
              "}\" : \"+v\"(" + Acc + ") : \"a\"(" + Address +
              "), \"Yk\"((uint16_t)(" + Mask + "))";
  for (unsigned I = 0; I < X86FourSourceCount; ++I)
    Fallback += ", \"v\"(" + T + "_r" + std::to_string(I) + ")";
  Fallback += std::string(" : \"memory\"") +
              (P.stackReference() ? ", \"rdx\", \"cc\"" : "") + "); })";
  Text += std::string(P.stackReference() ? "NEVERD_X86_FOUR_SOURCE_STACK("
                                         : "NEVERD_X86_FOUR_SOURCE(") +
          Native + ", " + Fallback + "); ";
  if (Live)
    Text += Type + " " + T + "_result; __builtin_memcpy(&" + T + "_result, &" +
            Acc + ", " + Bytes + "); " + T + "_result; ";
  else
    Text += "(void)0; ";
  return Text + "})";
}
} // namespace

X86FourSourcePlan x86FourSourceHighPlan(Arch A, const HighExpr &E) {
  X86FourSourceShape S;
  S.TargetArch = A;
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.ValidId = E.IntrinsicId == Intrinsic::X86FourSourceMemory &&
              E.Kind == ExprKind::Call && !E.IsIndirectCall &&
              !E.IndirectTarget && !E.SourceCallHint && !E.DoesNotReturn &&
              E.IntrinsicOutputs.empty();
  S.Arguments = E.Operands.size();
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.WritableOutput = E.Type && E.Type->Kind == NdTypeKind::Int;
  for (unsigned I = 0; I < S.Arguments && I < S.Sizes.size(); ++I) {
    const auto &V = E.Operands[I];
    S.Scalars[I] = V && V->Type &&
                   (V->Type->Kind == NdTypeKind::Int ||
                    (I == 0 && V->Type->Kind == NdTypeKind::Ptr));
    S.Sizes[I] = S.Scalars[I] ? V->Type->Size : 0;
    if (I == 4 && V && V->Kind == ExprKind::Const &&
        !isAddressProvenance(V->ConstProvenance))
      S.Control = V->ConstVal;
  }
  if ((A != Arch::X86 && A != Arch::X64) || !x86FourSourceShapeIsValid(S))
    throw std::invalid_argument(X86FourSourceMalformed);
  const auto C = *x86FourSourceControl(*S.Control);
  const auto Prefix = x86FourSourceNativePrefix(C, E.MemoryAddressSpace);
  return *x86FourSourcePlan(static_cast<unsigned>(C.Kind), C.Zero, Prefix);
}

std::optional<X86FourSourcePlan>
x86FourSourceCallPlan(Arch A, const llvm::CallInst &Call) {
  const auto *F = Call.getCalledFunction();
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  std::optional<X86FourSourcePlan> P;
  if (F)
    P = x86FourSourceNativeName(F->getName().str());
  if (P) {
    const auto ID = llvm::Intrinsic::lookupIntrinsicID(F->getName());
    if (!ID || !F->isDeclaration() || F->getIntrinsicID() != ID ||
        F->getFunctionType() !=
            llvm::Intrinsic::getType(Call.getContext(), ID) ||
        Call.getFunctionType() != F->getFunctionType() || Call.arg_size() != 9)
      throw std::invalid_argument(X86FourSourceMalformed);
    const auto Control = [&](unsigned I) -> uint64_t {
      const auto *V = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(I));
      if (!V || !V->getType()->isIntegerTy(32))
        throw std::invalid_argument(X86FourSourceMalformed);
      return V->getZExtValue();
    };
    P = x86FourSourcePlan(static_cast<unsigned>(P->Control.Kind), Control(7),
                          Control(8));
    if (!P)
      throw std::invalid_argument(X86FourSourceMalformed);
  } else if (Asm && llvm::StringRef(Asm->getAsmString())
                        .starts_with(X86FourSourceAsmMarker)) {
    std::istringstream S(
        Asm->getAsmString().substr(X86FourSourceAsmMarker.size()).str());
    unsigned Kind, Zero, Prefix;
    if (!(S >> Kind >> Zero >> Prefix))
      throw std::invalid_argument(X86FourSourceMalformed);
    P = x86FourSourcePlan(Kind, Zero, Prefix);
    if (!P || Asm->getAsmString() != P->assembly() ||
        (Asm->getConstraintString() != P->constraints() &&
         (P->stackReference() ||
          Asm->getConstraintString() != P->constraints(true))) ||
        !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT || Call.arg_size() != 7 ||
        Call.getFunctionType() != Asm->getFunctionType())
      throw std::invalid_argument(X86FourSourceMalformed);
  } else
    return {};
  const auto *V = llvm::FixedVectorType::get(
      llvm::Type::getInt32Ty(Call.getContext()), P->Control.bytes() / 4);
  if ((A != Arch::X86 && A != Arch::X64) ||
      (P->stackReference() && A != Arch::X64) ||
      Call.getFunctionType()->isVarArg() || Call.getNumOperandBundles() ||
      Call.doesNotReturn() || Call.hasFnAttr(llvm::Attribute::NoSync) ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.getCallingConv() != llvm::CallingConv::C || Call.getType() != V ||
      Call.getArgOperand(0)->getType() !=
          llvm::PointerType::getUnqual(Call.getContext()) ||
      !Call.getArgOperand(6)->getType()->isIntegerTy(16))
    throw std::invalid_argument(X86FourSourceMalformed);
  for (unsigned I = 1; I <= 5; ++I)
    if (Call.getArgOperand(I)->getType() != V)
      throw std::invalid_argument(X86FourSourceMalformed);
  for (unsigned I = 0; I < Call.arg_size(); ++I)
    if (Call.getFunctionType()->getParamType(I) !=
        Call.getArgOperand(I)->getType())
      throw std::invalid_argument(X86FourSourceMalformed);
  return P;
}

std::string renderX86FourSourceExpression(Arch A, const HighExpr &E,
                                          const std::vector<std::string> &Args,
                                          const std::string &Type) {
  const auto P = x86FourSourceHighPlan(A, E);
  if (Args.size() != 5)
    throw std::invalid_argument(X86FourSourceMalformed);
  const auto &Mask = *E.Operands[3];
  auto Use = x86FourSourceOperandUse(
      P.Control,
      Mask.Kind == ExprKind::Const && !isAddressProvenance(Mask.ConstProvenance)
          ? std::optional<uint64_t>(Mask.ConstVal)
          : std::nullopt);
  const auto CanOmit = [](const HighExpr &V) {
    return V.Kind == ExprKind::Const || V.Kind == ExprKind::Undef ||
           (V.Kind == ExprKind::Var &&
            (V.Var.Kind == MedVar::Reg || V.Var.Kind == MedVar::Temp ||
             V.Var.Kind == MedVar::Param));
  };
  Use.Address |= !CanOmit(*E.Operands[0]);
  Use.Sources |= !CanOmit(*E.Operands[2]);
  Use.Accumulator |= !CanOmit(*E.Operands[1]);
  return expression(P, {Args[0], Args[1], Args[2], Args[3]}, Type, true, Use);
}
std::optional<std::string> renderX86FourSourceCall(
    Arch A, const llvm::CallInst &Call, const std::vector<std::string> &Args,
    const std::string &Result, bool Live, const std::string &Type) {
  const auto P = x86FourSourceCallPlan(A, Call);
  if (!P)
    return {};
  if (Args.size() != Call.arg_size() || (Live && Result.empty()))
    throw std::invalid_argument(X86FourSourceMalformed);
  const auto *Mask = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(6));
  auto Use = x86FourSourceOperandUse(
      P->Control,
      Mask ? std::optional<uint64_t>(Mask->getZExtValue()) : std::nullopt);
  const auto CanOmit = [](const llvm::Value *V) {
    return llvm::isa<llvm::Constant, llvm::Argument>(V) ||
           llvm_source_unknown::isValue(V);
  };
  Use.Address |= !CanOmit(Call.getArgOperand(0));
  for (unsigned I : {2u, 3u, 4u, 5u})
    Use.Sources |= !CanOmit(Call.getArgOperand(I));
  Use.Accumulator |= !CanOmit(Call.getArgOperand(1));
  return (Live ? Result + " = " : "") +
         expression(*P, {Args.begin(), Args.begin() + 7}, Type, Live, Use) +
         ";\n";
}
} // namespace neverd
