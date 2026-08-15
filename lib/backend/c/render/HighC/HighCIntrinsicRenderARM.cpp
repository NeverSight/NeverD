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

namespace {

bool isAlive(const MedVar &V, const IsAliveFn &Fn) { return !Fn || Fn(V); }

std::string
renderMopsSetPrologue(Intrinsic IID, const std::vector<MedVar> &Outputs,
                      const std::vector<ExprPtr> &Operands,
                      std::function<std::string(const HighExpr &)> ExprFn,
                      std::function<std::string(const MedVar &)> VarFn,
                      const IsAliveFn &IsAlive) {
  if (Outputs.size() < 3 || Operands.size() < 3)
    return {};
  const char *Mnemonic = intrinsicAsmMnemonic(IID);
  if (!Mnemonic)
    return {};

  std::string Result = "{\n";
  Result += "        uint64_t _nd_mops_dst = (uint64_t)(uintptr_t)(" +
            ExprFn(*Operands[0]) + ");\n";
  Result += "        uint64_t _nd_mops_count = (uint64_t)(" +
            ExprFn(*Operands[1]) + ");\n";
  Result += "        uint64_t _nd_mops_nzcv;\n";
  Result += "        __asm__ volatile(\"" + std::string(Mnemonic) +
            " [%0]!, %1!, %3\\n\\tmrs %2, nzcv\"\n";
  Result += "                         : \"+r\"(_nd_mops_dst), "
            "\"+r\"(_nd_mops_count),\n";
  Result += "                           \"=r\"(_nd_mops_nzcv)\n";
  Result += "                         : \"r\"(" + ExprFn(*Operands[2]) + ")\n";
  Result += "                         : \"memory\", \"cc\");\n";
  const char *Names[] = {"_nd_mops_dst", "_nd_mops_count", "_nd_mops_nzcv"};
  for (size_t I = 0; I < 3; ++I)
    if (isAlive(Outputs[I], IsAlive))
      Result += "        " + VarFn(Outputs[I]) + " = " + Names[I] + ";\n";
  Result += "    }\n";
  return Result;
}

} // anonymous namespace

llvm::SmallVector<const char *, 3> getARMIntrinsicHeaders() {
  return {"arm_acle.h", "arm_neon.h"};
}

std::string
renderARMMultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive) {
  using I = Intrinsic;
  switch (IID) {
  case I::A64_MopsSetP:
  case I::A64_MopsSetPN:
  case I::A64_MopsSetPT:
  case I::A64_MopsSetPTN:
    return renderMopsSetPrologue(IID, Outputs, Operands, ExprFn, VarFn,
                                 IsAlive);
  default:
    return {};
  }
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
