//===- HighCIntrinsicRender.cpp - High IR intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Arch-dispatching entry points for HighIR intrinsic rendering.
/// Architecture-specific implementations live in
/// HighCIntrinsicRenderX86.cpp and HighCIntrinsicRenderARM.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"

#include <string>

namespace neverd {

std::string MultiOutputRender::operator()(
    Arch TheArch, Intrinsic IID, const std::vector<MedVar> &Outputs,
    const std::vector<ExprPtr> &Operands,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive) const {
  if (TheArch == Arch::ARM || TheArch == Arch::AArch64)
    return renderARMMultiOutput(IID, Outputs, Operands, ExprFn, VarFn, IsAlive);
  return renderX86MultiOutput(IID, Outputs, Operands, ExprFn, VarFn, IsAlive);
}

std::string renderIntrinsicCall(Intrinsic Id, Arch TheArch,
                                const std::vector<std::string> &Ops,
                                bool &HasCIntrinsics) {
  std::string Result;
  if (TheArch == Arch::ARM || TheArch == Arch::AArch64) {
    Result = renderARMIntrinsicCall(Id, Ops, HasCIntrinsics);
    if (!Result.empty())
      return Result;
  } else {
    Result = renderX86IntrinsicCall(Id, Ops, HasCIntrinsics);
    if (!Result.empty())
      return Result;
  }

  const char *CName = intrinsicCName(Id);
  if (CName) {
    std::string CallStr = std::string(CName) + "(";
    for (size_t I = 0; I < Ops.size(); ++I) {
      if (I > 0)
        CallStr += ", ";
      CallStr += Ops[I];
    }
    CallStr += ")";
    return CallStr;
  }

  const char *AsmMn = intrinsicAsmMnemonic(Id);
  if (AsmMn) {
    if (TheArch == Arch::X64 || TheArch == Arch::X86)
      return renderX86AsmStatement(AsmMn, Ops);
    return renderARMAsmStatement(AsmMn, Ops);
  }

  return {};
}

} // namespace neverd
