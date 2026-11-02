//===- X86BoundedLeaf.cpp - Bounded SGX/SMX C state transport ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Evaluate each operand once and validate complete native/assembly contracts.
//===----------------------------------------------------------------------===//

#include "X86BoundedLeaf.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>

namespace neverd {
namespace {
std::string prefix(const std::vector<std::string> &Arguments,
                   const std::vector<std::string> &Outputs) {
  std::string P = "__neverd_leaf_";
  for (;;) {
    bool Collision = false;
    for (const auto *List : {&Arguments, &Outputs})
      for (const auto &Text : *List)
        Collision |= Text.find(P) != std::string::npos;
    if (!Collision)
      return P;
    P += '_';
  }
}
std::vector<std::string> locals(const std::string &P) {
  return {P + "ax", P + "bx", P + "cx", P + "dx", P + "flags"};
}
std::string wordType(Arch A) {
  return A == Arch::X64 ? "uint64_t" : "uint32_t";
}
std::string declarations(Arch A, const std::vector<std::string> &Values) {
  std::string Text;
  for (unsigned I = 0; I < Values.size(); ++I)
    Text += (I == X86BoundedLeafRegisterCount ? "uint16_t" : wordType(A)) +
            std::string(" ") + Values[I] + "; ";
  return Text;
}
std::string dispatch(Arch A, Intrinsic Id,
                     const std::vector<std::string> &Outputs,
                     const std::vector<std::string> &Arguments) {
  const auto P = prefix(Arguments, Outputs);
  const auto Regs = P + "registers";
  const auto Flags = P + "status";
  std::string Native = "unsigned long long " + Regs + "[4]; unsigned short " +
                       Flags + "; __builtin_ia32_" +
                       (isX86EncluLeaf(Id) ? "enclu" : "getsec") +
                       "_bounded_state(";
  for (unsigned I = 0; I < Arguments.size(); ++I)
    Native += std::string(I == X86BoundedLeafRegisterCount
                              ? "(unsigned short)("
                              : "(unsigned long long)(") +
              Arguments[I] + "), ";
  Native += Regs + ", &" + Flags + "); ";
  for (unsigned I = 0; I < X86BoundedLeafRegisterCount; ++I)
    Native += Outputs[I] + " = " + Regs + "[" + std::to_string(I) + "]; ";
  Native += Outputs.back() + " = " + Flags + "; ";

  // No operand placeholders occur in this owned template: $$ quotes an
  // assembler immediate for LLVM, while C extended asm uses a single $.
  std::string Asm;
  const auto Template = x86BoundedLeafAsm(Id, A);
  for (size_t I = 0; I < Template.size(); ++I) {
    const char C = Template[I];
    if (C == '$' && I + 1 < Template.size() && Template[I + 1] == '$') {
      Asm += '$';
      ++I;
    } else
      Asm += C == '%'    ? "%%"
             : C == '\n' ? "\\n"
             : C == '\t' ? "\\t"
                         : std::string(1, C);
  }
  const auto Values = locals(P);
  std::string Fallback = declarations(A, Values);
  for (unsigned I = 0; I < Values.size(); ++I)
    Fallback += Values[I] + " = (" +
                (I == X86BoundedLeafRegisterCount ? "uint16_t" : wordType(A)) +
                ")(" + Arguments[I] + "); ";
  Fallback += "__asm__ __volatile__(\"" + Asm + "\" : ";
  const char *Constraints[] = {"+a", "+b", "+c", "+d", "+D"};
  for (unsigned I = 0; I < Values.size(); ++I) {
    if (I)
      Fallback += ", ";
    Fallback += std::string("\"") + Constraints[I] + "\"(" + Values[I] + ")";
  }
  Fallback += " : : \"esi\", \"cc\", \"memory\"); ";
  for (unsigned I = 0; I < Values.size(); ++I)
    Fallback += Outputs[I] + " = " + Values[I] + "; ";
  if (A == Arch::X86)
    return Fallback;
  return "NEVERD_X86_BOUNDED_LEAF(({ " + Native + "}), ({ " + Fallback +
         "})); ";
}
} // namespace

std::optional<Intrinsic> x86BoundedLeafCallId(Arch A,
                                              const llvm::CallInst &Call) {
  const auto *F = Call.getCalledFunction();
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  Intrinsic Id = Intrinsic::None;
  if (F && (F->getName().starts_with("llvm.x86.enclu.bounded.state") ||
            F->getName().starts_with("llvm.x86.getsec.bounded.state"))) {
    Id = F->getName().starts_with("llvm.x86.enclu")
             ? Intrinsic::X86EncluBounded
             : Intrinsic::X86GetsecBounded;
    const auto Native = llvm::Intrinsic::lookupIntrinsicID(F->getName());
    if (!Native || F->getName() != x86BoundedLeafNativeName(Id, A) ||
        !F->isDeclaration() || F->getIntrinsicID() != Native ||
        F->getFunctionType() !=
            llvm::Intrinsic::getType(Call.getContext(), Native) ||
        Call.getFunctionType() != F->getFunctionType())
      throw std::invalid_argument("invalid bounded leaf native declaration");
  } else if (Asm && llvm::StringRef(Asm->getAsmString())
                        .starts_with(X86BoundedLeafAsmMarker)) {
    const auto Name = llvm::StringRef(Asm->getAsmString())
                          .drop_front(X86BoundedLeafAsmMarker.size())
                          .split('\n')
                          .first;
    Id = Name == "enclu"    ? Intrinsic::X86EncluBounded
         : Name == "getsec" ? Intrinsic::X86GetsecBounded
                            : Intrinsic::None;
    if (Id == Intrinsic::None ||
        x86BoundedLeafAsm(Id, A) != Asm->getAsmString() ||
        Asm->getConstraintString() != X86BoundedLeafAsmConstraints ||
        !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT)
      throw std::invalid_argument("invalid owned bounded leaf assembly");
  } else
    return std::nullopt;
  const auto *Result = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if ((A != Arch::X86 && A != Arch::X64) || !Result || !Result->isLiteral() ||
      Result->isPacked() ||
      Result->getNumElements() != X86BoundedLeafRegisterCount + 1 ||
      Call.arg_size() != X86BoundedLeafRegisterCount + 1 ||
      Call.getFunctionType()->isVarArg() || Call.getNumOperandBundles() ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.doesNotReturn())
    throw std::invalid_argument("invalid bounded leaf state type or effects");
  for (unsigned I = 0; I < Call.arg_size(); ++I) {
    const unsigned Bits = I == X86BoundedLeafRegisterCount
                              ? 8 * X86ArithmeticFlagsBytes
                          : A == Arch::X64 ? 64
                                           : 32;
    if (!Call.getArgOperand(I)->getType()->isIntegerTy(Bits) ||
        !Result->getElementType(I)->isIntegerTy(Bits) ||
        Call.getFunctionType()->getParamType(I) !=
            Call.getArgOperand(I)->getType())
      throw std::invalid_argument("invalid bounded leaf operand width");
  }
  const auto *Ax = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(0));
  if (!Ax || !x86BoundedLeafContract(Id, Ax->getZExtValue()))
    throw std::invalid_argument(X86BoundedLeafUnproven);
  return Id;
}

std::string renderX86BoundedLeafExpression(
    Arch A, const HighExpr &E,
    const std::function<std::string(const HighExpr &)> &Expr,
    const std::function<std::string(const MedVar &)> &Var,
    const std::function<bool(const MedVar &)> &Alive) {
  X86BoundedLeafShape S;
  S.TargetArch = A;
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.ValidId = E.Kind == ExprKind::Call && !E.IsIndirectCall &&
              !E.IndirectTarget && !E.SourceCallHint && !E.DoesNotReturn &&
              E.IntrinsicOutputs.size() == X86BoundedLeafAuxiliaryOutputs;
  S.WritableOutput = E.Type && (E.Type->Kind == NdTypeKind::Int ||
                                E.Type->Kind == NdTypeKind::Ptr);
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.Arguments = E.Operands.size();
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &V = E.Operands[I];
    S.InputSizes[I] = V && V->Type ? V->Type->Size : 0;
    S.ScalarInputs[I] =
        V && V->Type &&
        (V->Type->Kind == NdTypeKind::Int || V->Type->Kind == NdTypeKind::Ptr);
  }
  if (!E.Operands.empty() && E.Operands[0] &&
      E.Operands[0]->Kind == ExprKind::Const &&
      !isAddressProvenance(E.Operands[0]->ConstProvenance))
    S.Ax = E.Operands[0]->ConstVal;
  if (isX86BoundedLeafPending(E.IntrinsicId) ||
      !x86BoundedLeafShapeIsValid(E.IntrinsicId, S))
    throw std::invalid_argument(X86BoundedLeafUnproven);
  std::vector<std::string> Arguments, Outputs;
  for (const auto &V : E.Operands)
    Arguments.push_back(Expr(*V));
  for (unsigned I = 0; I < E.IntrinsicOutputs.size(); ++I) {
    const auto &V = E.IntrinsicOutputs[I];
    if (V.Kind != MedVar::Temp || V.Id < 0 ||
        V.Size != (I < 3 ? S.OutputSize : X86ArithmeticFlagsBytes))
      throw std::invalid_argument("invalid bounded leaf auxiliary result");
    for (unsigned J = 0; J < I; ++J)
      if (E.IntrinsicOutputs[J].Id == V.Id)
        throw std::invalid_argument("duplicate bounded leaf auxiliary result");
    if (Alive(V))
      Outputs.push_back(Var(V));
  }
  const auto Values = locals(prefix(Arguments, Outputs));
  std::string Text = "({ " + declarations(A, Values) +
                     dispatch(A, E.IntrinsicId, Values, Arguments);
  for (unsigned I = 0; I < E.IntrinsicOutputs.size(); ++I)
    if (Alive(E.IntrinsicOutputs[I]))
      Text += Var(E.IntrinsicOutputs[I]) + " = " + Values[I + 1] + "; ";
  return Text + Values[0] + "; })";
}

std::optional<std::string>
renderX86BoundedLeafCall(Arch A, const llvm::CallInst &Call,
                         const std::string &Result, bool ResultLive,
                         const std::vector<std::string> &Arguments) {
  const auto Id = x86BoundedLeafCallId(A, Call);
  if (!Id)
    return std::nullopt;
  if (Arguments.size() != Call.arg_size() || (ResultLive && Result.empty()))
    throw std::invalid_argument("invalid bounded leaf result binding");
  auto Outputs = locals(prefix(Arguments, {Result}));
  std::string Text = "{ ";
  if (ResultLive)
    for (unsigned I = 0; I < Outputs.size(); ++I)
      Outputs[I] = Result + ".field_" + std::to_string(I);
  else
    Text += declarations(A, Outputs);
  return Text + dispatch(A, *Id, Outputs, Arguments) + "}\n";
}
} // namespace neverd
