//===- X86Sgx.cpp - SGX source completion -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native compiler calls and owned assembly share the same completion contract.
//===----------------------------------------------------------------------===//

#include "X86Sgx.h"

#include "neverd/backend/llvm/X86/X86SgxAsm.h"
#include "neverd/backend/llvm/X86/X86SgxNative.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace neverd {
namespace {

std::string wordType(const X86SgxAsm &P) {
  return "uint" + std::to_string(P.WordBytes * 8) + "_t";
}

std::string renderAsm(const X86SgxAsm &P,
                      const std::vector<std::string> &Outputs,
                      const std::vector<std::string> &Arguments,
                      bool FullRegisters = false) {
  std::string Text;
  for (const char C : P.Text)
    Text += C == '%'    ? "%%"
            : C == '$'  ? "%"
            : C == '\n' ? "\\n"
            : C == '\t' ? "\\t"
                        : std::string(1, C);
  auto Constraint = [](std::string C) {
    for (auto [Fixed, Letter] :
         {std::pair{"{ax}", "a"}, {"{bx}", "b"}, {"{cx}", "c"}, {"{dx}", "d"}})
      if (const auto At = C.find(Fixed); At != std::string::npos)
        C.replace(At, 4, Letter);
    return C;
  };
  std::vector<std::string> OutConstraints(P.OutputConstraints.begin(),
                                          P.OutputConstraints.end());
  std::vector<std::string> InConstraints(P.InputConstraints.begin(),
                                         P.InputConstraints.end());
  if (FullRegisters) {
    OutConstraints.insert(OutConstraints.begin() + 2, {"={cx}", "={dx}"});
    InConstraints[2] = "2";
    InConstraints[3] = "3";
  }
  Text = "__asm__ volatile(\"" + Text + "\" : ";
  for (unsigned I = 0; I < OutConstraints.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + Constraint(OutConstraints[I]) + "\"(" + Outputs[I] + ")";
  }
  Text += " : ";
  for (unsigned I = 0; I < InConstraints.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + Constraint(InConstraints[I]) + "\"((" + wordType(P) + ")(" +
            Arguments[I] + "))";
  }
  return Text + " : \"cc\", \"memory\"); ";
}

std::string localPrefix(const std::vector<std::string> &Arguments,
                        const std::vector<std::string> &Outputs = {}) {
  std::string Prefix = "neverd_sgx_";
  for (;;) {
    bool Collision = false;
    for (const auto *List : {&Arguments, &Outputs})
      for (const auto &Text : *List)
        Collision |= Text.find(Prefix) != std::string::npos;
    if (!Collision)
      return Prefix;
    Prefix += '_';
  }
}
std::vector<std::string> locals(const std::string &Prefix, bool Full) {
  if (Full)
    return {Prefix + "ax", Prefix + "bx", Prefix + "cx", Prefix + "dx",
            Prefix + "flags"};
  return {Prefix + "ax", Prefix + "bx", Prefix + "flags"};
}
std::string declarations(const X86SgxAsm &P,
                         const std::vector<std::string> &Locals) {
  std::string Text;
  for (unsigned I = 0; I + 1 < Locals.size(); ++I)
    Text += wordType(P) + " " + Locals[I] + "; ";
  return Text + "uint16_t " + Locals.back() + "; ";
}
std::string native(const X86SgxAsm &P, const std::vector<std::string> &Outputs,
                   const std::vector<std::string> &Arguments, bool Compact) {
  const auto Prefix = localPrefix(Arguments, Outputs);
  const auto Regs = Prefix + "registers";
  const auto Flags = Prefix + "status";
  std::string Text = "unsigned long long " + Regs + "[3]; unsigned char " +
                     Flags + "; " + Outputs[0] + " = __builtin_ia32_" +
                     P.Text.substr(0, P.Text.find('\n')) + "(";
  for (const auto &Arg : Arguments)
    Text += "(unsigned long long)(" + Arg + "), ";
  Text += Regs + ", &" + Flags + "); ";
  for (unsigned I = 1; I + 1 < Outputs.size(); ++I)
    Text += Outputs[I] + " = " + Regs + "[" + std::to_string(I - 1) + "]; ";
  Text += Outputs.back() + " = ";
  return Text +
         (Compact
              ? Flags
              : "(" + Flags + " & 1) | ((uint16_t)(" + Flags + " & 2) << 7)") +
         "; ";
}
std::string dispatch(const X86SgxAsm &P,
                     const std::vector<std::string> &Outputs,
                     const std::vector<std::string> &Arguments,
                     bool Compact = false) {
  std::string Fallback;
  if (Compact) {
    const auto Flags = localPrefix(Arguments, Outputs) + "status16";
    auto AssemblyOutputs = Outputs;
    AssemblyOutputs.back() = Flags;
    Fallback = "uint16_t " + Flags + "; " +
               renderAsm(P, AssemblyOutputs, Arguments, true) + Outputs.back() +
               " = (" + Flags + " & 1) | ((" + Flags + " >> 7) & 2); ";
  } else
    Fallback = renderAsm(P, Outputs, Arguments);
  // NeverC's native API is x64-only. i386 always uses the owned assembly.
  if (P.WordBytes != 8)
    return Fallback;
  return "NEVERD_X86_SGX(({ " + native(P, Outputs, Arguments, Compact) +
         "}), ({ " + Fallback + "})); ";
}
std::optional<Intrinsic> nativeId(const llvm::CallInst &Call, Arch A) {
  const auto *F = Call.getCalledFunction();
  if (!F || !isX86SgxNativeName(F->getName().str()))
    return {};
  const auto Id = F->getName().starts_with("llvm.x86.encls")
                      ? Intrinsic::X86Encls
                      : Intrinsic::X86Enclv;
  const auto ID = llvm::Intrinsic::lookupIntrinsicID(F->getName());
  if (F->getName() != x86SgxNativeName(Id, A) || !ID || !F->isDeclaration() ||
      F->getIntrinsicID() != ID ||
      F->getFunctionType() != llvm::Intrinsic::getType(Call.getContext(), ID) ||
      Call.getFunctionType() != F->getFunctionType() || Call.arg_size() != 4 ||
      Call.getNumOperandBundles() || Call.onlyReadsMemory() ||
      Call.onlyWritesMemory() || Call.doesNotReturn())
    throw std::invalid_argument("invalid SGX native type, mode or effects");
  for (unsigned I = 0; I < Call.arg_size(); ++I)
    if (Call.getArgOperand(I)->getType() !=
        F->getFunctionType()->getParamType(I))
      throw std::invalid_argument("invalid SGX native operand type");
  return Id;
}

} // namespace

std::string
renderX86SgxExpression(Arch A, const HighExpr &E,
                       const std::function<std::string(const HighExpr &)> &Expr,
                       const std::function<std::string(const MedVar &)> &Var,
                       const std::function<bool(const MedVar &)> &Alive) {
  X86SgxShape S;
  S.TargetArch = A;
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.ValidId = E.Kind == ExprKind::Call && !E.IsIndirectCall &&
              !E.IndirectTarget && !E.SourceCallHint && !E.DoesNotReturn &&
              E.IntrinsicOutputs.size() == X86SgxAuxiliaryOutputs;
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
  if (!x86SgxShapeIsValid(E.IntrinsicId, S))
    throw std::invalid_argument("invalid HighC SGX completion contract");
  const auto P = x86SgxAsm(E.IntrinsicId, A);
  std::vector<std::string> Arguments;
  for (const auto &V : E.Operands)
    Arguments.push_back(Expr(*V));
  std::vector<std::string> OutputNames;
  for (const auto &V : E.IntrinsicOutputs)
    if (Alive(V))
      OutputNames.push_back(Var(V));
  const auto Locals = locals(localPrefix(Arguments, OutputNames), false);
  std::string Text =
      "({ " + declarations(*P, Locals) + dispatch(*P, Locals, Arguments);
  for (unsigned I = 0; I < X86SgxAuxiliaryOutputs; ++I) {
    const auto &V = E.IntrinsicOutputs[I];
    if (V.Kind != MedVar::Temp || V.Id < 0 ||
        V.Size != (I == 0 ? P->WordBytes : X86SgxFlagBytes))
      throw std::invalid_argument("invalid HighC SGX auxiliary result");
    if (Alive(V))
      Text += Var(V) + " = " + Locals[I + 1] + "; ";
  }
  return Text + Locals[0] + "; })";
}

std::optional<std::string>
renderX86SgxCall(Arch A, const llvm::CallInst &Call, const std::string &Result,
                 bool ResultLive, const std::vector<std::string> &Arguments) {
  if (const auto Id = nativeId(Call, A)) {
    if (Arguments.size() != Call.arg_size() || (ResultLive && Result.empty()))
      throw std::invalid_argument("invalid SGX native result");
    const auto P = *x86SgxAsm(*Id, A);
    auto Outputs = locals(localPrefix(Arguments), true);
    std::string Text = "{ ";
    if (ResultLive)
      for (unsigned I = 0; I < Outputs.size(); ++I)
        Outputs[I] = Result + ".field_" + std::to_string(I);
    else
      Text += declarations(P, Outputs);
    return Text + dispatch(P, Outputs, Arguments, true) + "}\n";
  }
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm)
    return {};
  const auto Mnemonic = llvm::StringRef(Asm->getAsmString()).split('\n').first;
  const auto Id = Mnemonic == "encls"   ? Intrinsic::X86Encls
                  : Mnemonic == "enclv" ? Intrinsic::X86Enclv
                                        : Intrinsic::None;
  if (Id == Intrinsic::None)
    return {};
  const auto P = x86SgxAsm(Id, A);
  const auto *R = llvm::dyn_cast<llvm::StructType>(Call.getType());
  bool Valid =
      P && Asm->getAsmString() == P->Text &&
      Asm->getConstraintString() == P->Constraints && Asm->hasSideEffects() &&
      !Asm->isAlignStack() && !Asm->canThrow() &&
      Asm->getDialect() == llvm::InlineAsm::AD_ATT &&
      !Call.getNumOperandBundles() && !Call.onlyReadsMemory() &&
      !Call.onlyWritesMemory() && !Call.doesNotReturn() &&
      !Call.getFunctionType()->isVarArg() &&
      Call.arg_size() == P->InputConstraints.size() &&
      Arguments.size() == Call.arg_size() && R && R->isLiteral() &&
      !R->isPacked() && R->getNumElements() == P->OutputConstraints.size();
  if (Valid) {
    Valid &= R->getElementType(0)->isIntegerTy(8 * P->WordBytes) &&
             R->getElementType(1)->isIntegerTy(8 * P->WordBytes) &&
             R->getElementType(2)->isIntegerTy(8 * X86SgxFlagBytes);
    for (const auto &V : Call.args())
      Valid &= V->getType()->isIntegerTy(8 * P->WordBytes);
  }
  if (!Valid || (ResultLive && Result.empty()))
    throw std::invalid_argument("unsupported SGX inline assembly contract");
  if (ResultLive)
    return dispatch(
               *P,
               {Result + ".field_0", Result + ".field_1", Result + ".field_2"},
               Arguments) +
           '\n';
  const auto Locals = locals(localPrefix(Arguments), false);
  return "{ " + declarations(*P, Locals) + dispatch(*P, Locals, Arguments) +
         "}\n";
}

} // namespace neverd
