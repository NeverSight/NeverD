//===- HighCIntrinsicRenderARM.cpp - ARM intrinsic rendering ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM/AArch64-specific HighIR intrinsic rendering: ACLE barrier and
/// exclusive-monitor intrinsics (DMB, DSB, ISB, CLREX).
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"

namespace neverd {

llvm::SmallVector<const char *, 3> getARMIntrinsicHeaders() {
  return {"arm_acle.h", "arm_neon.h"};
}

std::string
renderARMMultiOutput(Intrinsic /*IID*/, const std::vector<MedVar> & /*Outputs*/,
                     const std::vector<ExprPtr> & /*Operands*/,
                     std::function<std::string(const HighExpr &)> /*ExprFn*/,
                     std::function<std::string(const MedVar &)> /*VarFn*/,
                     IsAliveFn /*IsAlive*/) {
  return {};
}

std::string renderARMIntrinsicCall(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Dmb:
  case I::ArmDmb:
    return "__dmb(0xF)";
  case I::Dsb:
  case I::ArmDsb:
    return "__dsb(0xB)";
  case I::Isb:
  case I::ArmIsb:
    return "__isb(0xF)";
  case I::A64_Clrex:
  case I::ArmClrex:
    return "__clrex()";
  default:
    return {};
  }
}

std::string renderARMAsmStatement(const char *Mnemonic,
                                  const std::vector<std::string> &Ops) {
  if (Ops.empty())
    return std::string("__asm__ volatile(\"") + Mnemonic + "\" ::: \"memory\")";
  std::string Inputs;
  for (size_t I = 0; I < Ops.size(); ++I) {
    if (I > 0)
      Inputs += ", ";
    Inputs += "\"r\"(" + Ops[I] + ")";
  }
  return std::string("__asm__ volatile(\"") + Mnemonic + "\" : : " + Inputs +
         " : \"memory\")";
}

} // namespace neverd
