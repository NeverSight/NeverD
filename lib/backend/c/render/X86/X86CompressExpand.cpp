//===- X86CompressExpand.cpp - Compact memory C projection --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check full contracts before rendering builtins or owned inline assembly.
//===----------------------------------------------------------------------===//
#include "X86CompressExpand.h"

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
std::string expression(const X86CompactPlan &P,
                       const std::vector<std::string> &Args,
                       const std::string &Type, bool Live,
                       const X86CompactOperandUse &Use,
                       const std::string &InputType) {
  if (Args.size() != 3 || (P.Expand && Live && Type.empty()))
    throw std::invalid_argument(X86CompactMalformed);
  std::string T = "neverd_compact";
  while (std::any_of(Args.begin(), Args.end(), [&](const auto &Arg) {
    return Arg.find(T) != std::string::npos;
  }))
    T += '_';
  const auto V = T + "_vector", Input = T + "_input", Output = T + "_output";
  const auto Address =
      Use.Address ? "(void *)(uintptr_t)(" + Args[0] + ")" : "(void *)0";
  const auto Vector = "unsigned int " + V + " __attribute__((vector_size(" +
                      std::to_string(P.Bytes) + ")))";
  std::string Text = "({ ";
  if (Use.Data)
    Text += InputType + " " + Input + " = (" + Args[2] + "); " + Vector +
            "; __builtin_memcpy(&" + V + ", &" + Input + ", " +
            std::to_string(P.Bytes) + "); ";
  else
    Text += Vector + " = {0}; ";
  std::string Native = (P.Expand ? V + " = " : "") + P.builtin() + "(" +
                       Address + ", (unsigned long long)(" + Args[1] + "), " +
                       V + ", " +
                       (P.Expand ? std::to_string(P.Zero) + ", " : "") +
                       std::to_string(P.Prefix) + ")";
  const auto Segment = P.hasSegmentPrefix() ? "%%" + P.segment() + ":" : "";
  // GNU assembly alternatives support both -masm=att and -masm=intel.
  const auto Mask = P.Expand ? "2" : "1";
  const auto AttMem = Segment + (P.stackReference() ? "(%%rbp,%%rdx)"
                                 : P.Expand         ? "(%1)"
                                                    : "(%0)");
  const auto IntelMem = (P.hasSegmentPrefix() ? P.segment() + ":" : "") +
                        (P.stackReference() ? "[rbp+rdx]"
                         : P.Expand         ? "[%1]"
                                            : "[%0]");
  const auto Suffix = " %{%" + std::string(Mask) + "%}";
  const auto Att = P.Expand
                       ? AttMem + ", %0" + Suffix + (P.Zero ? " %{z%}" : "")
                       : "%2, " + AttMem + Suffix;
  const auto Intel =
      P.Expand ? "%0" + Suffix + (P.Zero ? " %{z%}" : "") + ", " + IntelMem
               : IntelMem + Suffix + ", %2";
  const auto Setup = P.stackReference()
                         ? std::string("mov{q %q") + (P.Expand ? "1" : "0") +
                               ", %%rdx| rdx, %q" + (P.Expand ? "1" : "0") +
                               "}\\n\\tsub{q %%rbp, %%rdx| rdx, rbp}\\n\\t"
                         : "";
  std::string Fallback = "__asm__ __volatile__(\"" + Setup + P.mnemonic() +
                         " {" + Att + "|" + Intel + "}\" : ";
  if (P.Expand)
    Fallback += "\"+v\"(" + V + ")";
  Fallback += " : \"a\"(" + Address + "), \"Yk\"((uint" +
              std::to_string(P.maskBits()) + "_t)(" + Args[1] + "))";
  if (!P.Expand)
    Fallback += ", \"v\"(" + V + ")";
  Fallback += std::string(" : \"memory\"") +
              (P.stackReference() ? ", \"rdx\", \"cc\"" : "") + ")";
  Text += std::string(P.stackReference() ? "NEVERD_X86_COMPACT_STACK("
                                         : "NEVERD_X86_COMPACT(") +
          Native + ", " + Fallback + "); ";
  if (P.Expand && Live)
    Text += Type + " " + Output + "; __builtin_memcpy(&" + Output + ", &" + V +
            ", " + std::to_string(P.Bytes) + "); " + Output + "; ";
  else
    Text += "(void)0; ";
  return Text + "})";
}
} // namespace
std::vector<std::string> x86CompactSourceFeatures(const X86CompactPlan &P) {
  auto Features = P.features();
  // C frontends enable 512-bit EVEX through AVX512F. The backend-only
  // evex512 spelling is not a portable target attribute (Clang rejects it).
  Features.erase(std::remove(Features.begin(), Features.end(), "evex512"),
                 Features.end());
  return Features;
}
X86CompactPlan x86CompactHighPlan(Arch A, const HighExpr &E) {
  X86CompactShape S;
  S.TargetArch = A;
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.ValidId = isX86CompactMemory(E.IntrinsicId) && E.Kind == ExprKind::Call &&
              !E.IsIndirectCall && !E.IndirectTarget && !E.SourceCallHint &&
              !E.DoesNotReturn && E.IntrinsicOutputs.empty();
  S.Arguments = E.Operands.size();
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.WritableOutput = E.Type && E.Type->Kind == NdTypeKind::Int;
  for (unsigned I = 0; I < S.Arguments && I < S.Sizes.size(); ++I) {
    const auto &V = E.Operands[I];
    S.Scalars[I] = V && V->Type &&
                   (V->Type->Kind == NdTypeKind::Int ||
                    (I == 0 && V->Type->Kind == NdTypeKind::Ptr));
    S.Sizes[I] = S.Scalars[I] ? V->Type->Size : 0;
    if (I == 3 && V && V->Kind == ExprKind::Const &&
        !isAddressProvenance(V->ConstProvenance))
      S.Control = V->ConstVal;
  }
  if ((A != Arch::X64 && A != Arch::X86) ||
      !x86CompactShapeIsValid(E.IntrinsicId, S))
    throw std::invalid_argument(X86CompactMalformed);
  const auto C = *x86CompactControl(E.IntrinsicId, *S.Control);
  const unsigned Prefix = x86CompactNativePrefix(C, E.MemoryAddressSpace);
  return *x86CompactPlan(E.IntrinsicId == Intrinsic::EVEXExpandLoad, S.Sizes[2],
                         C.Kind, C.Zero, Prefix);
}
std::optional<X86CompactPlan> x86CompactCallPlan(Arch A,
                                                 const llvm::CallInst &Call) {
  const auto *F = Call.getCalledFunction();
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  std::optional<X86CompactPlan> P;
  if (F)
    P = x86CompactNativeName(F->getName().str());
  if (P) {
    const auto ID = llvm::Intrinsic::lookupIntrinsicID(F->getName());
    if (!ID || !F->isDeclaration() || F->getIntrinsicID() != ID ||
        F->getFunctionType() !=
            llvm::Intrinsic::getType(Call.getContext(), ID) ||
        Call.getFunctionType() != F->getFunctionType() ||
        Call.arg_size() != (P->Expand ? 6u : 5u))
      throw std::invalid_argument(X86CompactMalformed);
    const auto Control = [&](unsigned I) -> uint64_t {
      const auto *V = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(I));
      if (!V || !V->getType()->isIntegerTy(32))
        throw std::invalid_argument(X86CompactMalformed);
      return V->getZExtValue();
    };
    P = x86CompactPlan(P->Expand, P->Bytes, Control(3),
                       P->Expand ? Control(4) : 0, Control(P->Expand ? 5 : 4));
    if (!P)
      throw std::invalid_argument(X86CompactMalformed);
  } else if (Asm && llvm::StringRef(Asm->getAsmString())
                        .starts_with(X86CompactAsmMarker)) {
    std::istringstream S(
        Asm->getAsmString().substr(X86CompactAsmMarker.size()).str());
    unsigned Expand, Bytes, Kind, Zero, Prefix;
    if (!(S >> Expand >> Bytes >> Kind >> Zero >> Prefix) || Expand > 1)
      throw std::invalid_argument(X86CompactMalformed);
    P = x86CompactPlan(Expand, Bytes, Kind, Zero, Prefix);
    if (!P || Asm->getAsmString() != P->assembly() ||
        (Asm->getConstraintString() != P->constraints() &&
         (P->stackReference() ||
          Asm->getConstraintString() != P->constraints(true))) ||
        !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT || Call.arg_size() != 3 ||
        Call.getFunctionType() != Asm->getFunctionType())
      throw std::invalid_argument(X86CompactMalformed);
  } else
    return {};
  auto *V = llvm::FixedVectorType::get(
      llvm::Type::getInt32Ty(Call.getContext()), P->Bytes / 4);
  if ((A != Arch::X64 && A != Arch::X86) ||
      (P->stackReference() && A != Arch::X64) ||
      Call.getFunctionType()->isVarArg() || Call.getNumOperandBundles() ||
      Call.doesNotReturn() || Call.hasFnAttr(llvm::Attribute::NoSync) ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      (P->Expand ? Call.getType() != V : !Call.getType()->isVoidTy()) ||
      Call.getArgOperand(0)->getType() !=
          llvm::PointerType::getUnqual(Call.getContext()) ||
      !Call.getArgOperand(1)->getType()->isIntegerTy(Asm ? P->maskBits()
                                                         : 64) ||
      Call.getArgOperand(2)->getType() != V)
    throw std::invalid_argument(X86CompactMalformed);
  for (unsigned I = 0; I < Call.arg_size(); ++I)
    if (Call.getFunctionType()->getParamType(I) !=
        Call.getArgOperand(I)->getType())
      throw std::invalid_argument(X86CompactMalformed);
  return P;
}
std::string renderX86CompactExpression(Arch A, const HighExpr &E,
                                       const std::vector<std::string> &Args,
                                       const std::string &Type) {
  const auto P = x86CompactHighPlan(A, E);
  if (Args.size() != 4)
    throw std::invalid_argument(X86CompactMalformed);
  const auto &Mask = *E.Operands[1];
  const auto Known =
      Mask.Kind == ExprKind::Const && !isAddressProvenance(Mask.ConstProvenance)
          ? std::optional<uint64_t>(Mask.ConstVal)
          : std::nullopt;
  auto Use =
      x86CompactOperandUse(P.Bytes / P.elementBytes(), P.Expand, P.Zero, Known);
  const auto CanOmit = [](const HighExpr &V) {
    return V.Kind == ExprKind::Const || V.Kind == ExprKind::Undef ||
           (V.Kind == ExprKind::Var &&
            (V.Var.Kind == MedVar::Reg || V.Var.Kind == MedVar::Temp ||
             V.Var.Kind == MedVar::Param));
  };
  // Unread register values need no observation. Independent loads/calls in
  // an expression must retain their own faults and effects, even when the
  // compact instruction does not consume the resulting value.
  Use.Address |= !CanOmit(*E.Operands[0]);
  Use.Data |= !CanOmit(*E.Operands[2]);
  // A wide HighIR constant may print as an ordinary C integer literal. Its
  // storage must still cover the complete packed input copied below.
  return expression(P, {Args[0], Args[1], Args[2]}, Type, P.Expand, Use,
                    "unsigned _BitInt(" + std::to_string(P.Bytes * 8) + ")");
}
std::optional<std::string> renderX86CompactCall(
    Arch A, const llvm::CallInst &Call, const std::vector<std::string> &Args,
    const std::string &Result, bool Live, const std::string &Type) {
  const auto P = x86CompactCallPlan(A, Call);
  if (!P)
    return {};
  if (Args.size() != Call.arg_size() ||
      (Live && (!P->Expand || Result.empty())))
    throw std::invalid_argument(X86CompactMalformed);
  const auto *Mask = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1));
  auto Use = x86CompactOperandUse(
      P->Bytes / P->elementBytes(), P->Expand, P->Zero,
      Mask ? std::optional<uint64_t>(Mask->getZExtValue()) : std::nullopt);
  const auto CanOmit = [](const llvm::Value *V) {
    return llvm::isa<llvm::Constant, llvm::Argument>(V) ||
           llvm_source_unknown::isValue(V);
  };
  Use.Address |= !CanOmit(Call.getArgOperand(0));
  Use.Data |= !CanOmit(Call.getArgOperand(2));
  return (Live ? Result + " = " : "") +
         expression(*P, {Args[0], Args[1], Args[2]}, Type, Live, Use,
                    "__auto_type") +
         ";\n";
}
} // namespace neverd
