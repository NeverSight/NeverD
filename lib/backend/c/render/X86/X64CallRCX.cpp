//===- X64CallRCX.cpp - Occurrence-bound native register results ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind the actual RCX from the same call that produces RAX. Each argument
/// is evaluated once, before entering assembly; there is no replay or helper
/// call between the native callee and its captured register results.
//===----------------------------------------------------------------------===//

#include "X64CallRCX.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"

#include <array>
#include <stdexcept>

namespace neverd {
namespace {
std::string prefix(const std::vector<std::string> &Inputs,
                   const std::string &Result) {
  std::string P = "__neverd_call_";
  for (;;) {
    bool Collision = Result.find(P) != std::string::npos;
    for (const auto &Input : Inputs)
      Collision |= Input.find(P) != std::string::npos;
    if (!Collision)
      return P;
    P += '_';
  }
}

std::string assembly(const std::string &P,
                     const std::vector<std::string> &Inputs) {
  const char *Names[] = {"ax", "cx", "dx", "r8", "r9"};
  const char *Constraints[] = {"+a", "+c", "+d", "+r", "+r"};
  std::string Text;
  // Argument expressions can contain calls. Evaluate them before binding
  // any volatile fixed-register variable used by the extended assembly.
  for (unsigned I = 0; I < X64CallRCXArguments; ++I)
    Text += "uint64_t " + P + "input_" + std::to_string(I) + " = (uint64_t)(" +
            Inputs[I] + "); ";
  for (unsigned I = 0; I < X64CallRCXArguments; ++I) {
    const bool Fixed = I >= 3;
    Text += std::string(Fixed ? "register " : "") + "uint64_t " + P + Names[I];
    if (Fixed)
      Text += std::string(" __asm__(\"") + Names[I] + "\")";
    Text += " = " + P + "input_" + std::to_string(I) + "; ";
  }
  std::string Asm;
  const std::string Template = X64CallRCXAsm;
  for (size_t I = 0; I < Template.size(); ++I) {
    const auto C = Template[I];
    if (C == '$' && I + 1 < Template.size() && Template[I + 1] == '$') {
      Asm += '$';
      ++I;
    } else
      Asm += C == '%'    ? "%%"
             : C == '\n' ? "\\n"
             : C == '\t' ? "\\t"
                         : std::string(1, C);
  }
  Text += "__asm__ __volatile__(\"" + Asm + "\" : ";
  for (unsigned I = 0; I < X64CallRCXArguments; ++I) {
    if (I)
      Text += ", ";
    Text += std::string("\"") + Constraints[I] + "\"(" + P + Names[I] + ")";
  }
  return Text + " : : " + X64CallRCXCClobbers + "); ";
}
} // namespace

bool isX64CallRCX(Arch A, const llvm::CallInst &Call) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || !llvm::StringRef(Asm->getAsmString())
                   .starts_with("# neverd.win64.call.rcx."))
    return false;
  const auto *Result = llvm::dyn_cast<llvm::StructType>(Call.getType());
  const auto *Owner = Call.getFunction();
  if (A != Arch::X64 || Asm->getAsmString() != X64CallRCXAsm ||
      Asm->getConstraintString() != X64CallRCXConstraints ||
      !Asm->hasSideEffects() || Asm->isAlignStack() || !Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT || !Result ||
      !Result->isLiteral() || Result->isPacked() ||
      Result->getNumElements() != X64CallRCXNativeResults ||
      Call.arg_size() != X64CallRCXArguments ||
      Call.getFunctionType()->isVarArg() || Call.getNumOperandBundles() ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.doesNotReturn() || Call.doesNotThrow() || Call.isMustTailCall() ||
      !Owner || !Owner->hasFnAttribute(X64CallRCXProfileAttribute) ||
      !Call.getModule()->getTargetTriple().isOSBinFormatCOFF() ||
      Owner->hasFnAttribute(llvm::Attribute::NoUnwind) ||
      !Owner->hasFnAttribute(llvm::Attribute::NoRedZone) ||
      Owner->getFnAttribute("frame-pointer").getValueAsString() != "all" ||
      Owner->getFnAttribute("target-cpu").getValueAsString() !=
          X64CallRCXTargetCPU ||
      Owner->getFnAttribute("target-features").getValueAsString() !=
          X64CallRCXTargetFeatures)
    throw std::invalid_argument(X64CallRCXDiagnostic);
  for (unsigned I = 0; I < X64CallRCXArguments; ++I)
    if (!Call.getArgOperand(I)->getType()->isIntegerTy(64) ||
        !Result->getElementType(I)->isIntegerTy(64) ||
        !Call.getFunctionType()->getParamType(I)->isIntegerTy(64))
      throw std::invalid_argument(X64CallRCXDiagnostic);
  return true;
}

std::string renderX64CallRCXExpression(
    Arch A, const HighExpr &E,
    const std::function<std::string(const HighExpr &)> &Expr,
    const std::function<std::string(const MedVar &)> &Var,
    const std::function<bool(const MedVar &)> &Alive) {
  X64CallRCXShape S;
  S.Architecture = A;
  S.Ordering = E.MemoryOrdering;
  S.AddressSpace = E.MemoryAddressSpace;
  S.ValidId = E.Kind == ExprKind::Call &&
              E.IntrinsicId == Intrinsic::X64CallRCX && !E.IsIndirectCall &&
              !E.IndirectTarget && !E.SourceCallHint && !E.DoesNotReturn;
  S.WritableOutput = E.Type && (E.Type->Kind == NdTypeKind::Int ||
                                E.Type->Kind == NdTypeKind::Ptr);
  S.OutputBytes = E.Type ? E.Type->Size : 0;
  S.Arguments = E.Operands.size();
  for (unsigned I = 0; I < S.Arguments && I < S.InputBytes.size(); ++I) {
    const auto &V = E.Operands[I];
    S.InputBytes[I] = V && V->Type ? V->Type->Size : 0;
    S.ScalarInputs[I] =
        V && V->Type &&
        (V->Type->Kind == NdTypeKind::Int || V->Type->Kind == NdTypeKind::Ptr);
  }
  if (E.IntrinsicOutputs.size() == 1) {
    const auto &V = E.IntrinsicOutputs.front();
    S.WritableRCX = V.Kind == MedVar::Temp && V.Id >= 0;
    S.RCXBytes = V.Size;
  }
  if (!x64CallRCXShapeIsValid(S))
    throw std::invalid_argument(X64CallRCXDiagnostic);
  const auto &RCX = E.IntrinsicOutputs.front();
  const auto Result = Alive(RCX) ? Var(RCX) : std::string();
  std::vector<std::string> Inputs;
  for (const auto &V : E.Operands)
    Inputs.push_back(Expr(*V));
  const auto P = prefix(Inputs, Result);
  std::string Text = "({ " + assembly(P, Inputs);
  if (!Result.empty())
    Text += Result + " = " + P + "cx; ";
  return Text + P + "ax; })";
}

std::optional<std::string>
renderX64CallRCX(Arch A, const llvm::CallInst &Call, const std::string &Result,
                 bool ResultLive, const std::vector<std::string> &Arguments) {
  if (!isX64CallRCX(A, Call))
    return std::nullopt;
  if (Arguments.size() != X64CallRCXArguments || (ResultLive && Result.empty()))
    throw std::invalid_argument(X64CallRCXDiagnostic);
  const auto P = prefix(Arguments, Result);
  std::string Text = "{ " + assembly(P, Arguments);
  if (ResultLive) {
    const char *Names[] = {"ax", "cx", "dx", "r8", "r9"};
    for (unsigned I = 0; I < X64CallRCXNativeResults; ++I)
      Text +=
          Result + ".field_" + std::to_string(I) + " = " + P + Names[I] + "; ";
  }
  return Text + "}\n";
}
} // namespace neverd
