//===- HighCIntrinsicRenderX86.cpp - x86 intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific HighIR intrinsic rendering: multi-output CPUID/RDTSC/XGETBV
/// emission, single-output x86 intrinsic calls, and hi/lo collapse patterns.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"

#include <string>

namespace neverd {

llvm::SmallVector<const char *, 3> getX86IntrinsicHeaders() {
  return {"immintrin.h"};
}

namespace {

bool isAlive(const MedVar &V, const IsAliveFn &Fn) { return !Fn || Fn(V); }

std::string renderCpuid(const std::vector<MedVar> &Outs,
                        const std::vector<ExprPtr> &Ops,
                        std::function<std::string(const HighExpr &)> ExprFn,
                        std::function<std::string(const MedVar &)> VarFn,
                        const IsAliveFn &IsAlive) {
  std::string Leaf = Ops.empty() ? "0" : ExprFn(*Ops[0]);
  std::string Result = "int cpuInfo[4];\n";
  Result += "    __cpuid(cpuInfo, " + Leaf + ");\n";
  const char *Names[] = {"cpuInfo[0]", "cpuInfo[1]", "cpuInfo[2]",
                         "cpuInfo[3]"};
  for (size_t I = 0; I < Outs.size() && I < 4; ++I)
    if (isAlive(Outs[I], IsAlive))
      Result += "    " + VarFn(Outs[I]) + " = " + Names[I] + ";\n";
  return Result;
}

std::string renderXgetbv(const std::vector<MedVar> &Outs,
                         const std::vector<ExprPtr> &Ops,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         std::function<std::string(const MedVar &)> VarFn,
                         const IsAliveFn &IsAlive) {
  std::string ECX = Ops.empty() ? "0" : ExprFn(*Ops[0]);
  if (Outs.size() < 2)
    return "_xgetbv(" + ECX + ");\n";
  bool LoAlive = isAlive(Outs[0], IsAlive);
  bool HiAlive = isAlive(Outs[1], IsAlive);
  if (!LoAlive && !HiAlive)
    return "_xgetbv(" + ECX + ");\n";
  std::string Result = "uint64_t _xcr = _xgetbv(" + ECX + ");\n";
  if (LoAlive)
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_xcr;\n";
  if (HiAlive)
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_xcr >> 32);\n";
  return Result;
}

std::string renderRdtsc(const std::vector<MedVar> &Outs, const char *FnName,
                        std::function<std::string(const HighExpr &)> /*ExprFn*/,
                        std::function<std::string(const MedVar &)> VarFn,
                        const IsAliveFn &IsAlive) {
  if (Outs.size() < 2)
    return std::string("__") + FnName + "();\n";
  bool LoAlive = isAlive(Outs[0], IsAlive);
  bool HiAlive = isAlive(Outs[1], IsAlive);
  if (!LoAlive && !HiAlive)
    return std::string("__") + FnName + "();\n";
  std::string Result = std::string("uint64_t _tsc = __") + FnName + "();\n";
  if (LoAlive)
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_tsc;\n";
  if (HiAlive)
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_tsc >> 32);\n";
  return Result;
}

std::string renderRdtscp(const std::vector<MedVar> &Outs,
                         const std::vector<ExprPtr> & /*Ops*/,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         std::function<std::string(const MedVar &)> VarFn,
                         const IsAliveFn &IsAlive) {
  if (Outs.size() < 3)
    return renderRdtsc(Outs, "rdtscp", ExprFn, VarFn, IsAlive);
  std::string Result = "uint32_t _aux;\n";
  Result += "    uint64_t _tsc = __rdtscp(&_aux);\n";
  if (isAlive(Outs[0], IsAlive))
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_tsc;\n";
  if (isAlive(Outs[1], IsAlive))
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_tsc >> 32);\n";
  if (isAlive(Outs[2], IsAlive))
    Result += "    " + VarFn(Outs[2]) + " = _aux;\n";
  return Result;
}

} // anonymous namespace

std::string
renderX86MultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive) {
  using I = Intrinsic;
  switch (IID) {
  case I::Cpuid:
    return renderCpuid(Outputs, Operands, ExprFn, VarFn, IsAlive);
  case I::Xgetbv:
    return renderXgetbv(Outputs, Operands, ExprFn, VarFn, IsAlive);
  case I::Rdtsc:
    return renderRdtsc(Outputs, "rdtsc", ExprFn, VarFn, IsAlive);
  case I::Rdtscp:
    return renderRdtscp(Outputs, Operands, ExprFn, VarFn, IsAlive);
  default:
    return {};
  }
}

std::string renderX86IntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   bool &HasCIntrinsics) {
  using I = Intrinsic;
  switch (Id) {
  case I::Cpuid: {
    std::string Leaf = Ops.empty() ? "0" : Ops[0];
    HasCIntrinsics = true;
    return "{{ int cpuInfo[4]; __cpuid(cpuInfo, " + Leaf + "); }}";
  }
  case I::Rdtscp: {
    HasCIntrinsics = true;
    return "({ uint32_t _aux; uint64_t _tsc = __rdtscp(&_aux); "
           "_tsc; })";
  }
  default:
    return {};
  }
}

const char *hiloCollapseExpr(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Rdtsc:
    return "__rdtsc()";
  case I::Rdtscp:
    return "__rdtscp()";
  default:
    return nullptr;
  }
}

std::string renderX86AsmStatement(const char *Mnemonic,
                                  const std::vector<std::string> &Ops) {
  std::string AsmStmt = Mnemonic;
  for (size_t I = 0; I < Ops.size(); ++I) {
    AsmStmt += (I == 0 ? " " : ", ");
    AsmStmt += Ops[I];
  }
  return "__asm {{ " + AsmStmt + " }}";
}

} // namespace neverd
