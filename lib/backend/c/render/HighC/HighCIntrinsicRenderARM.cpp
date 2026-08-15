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

std::string
renderMopsCopyPrologue(const std::vector<MedVar> &Outputs,
                       const std::vector<ExprPtr> &Operands,
                       std::function<std::string(const HighExpr &)> ExprFn,
                       std::function<std::string(const MedVar &)> VarFn,
                       const IsAliveFn &IsAlive) {
  if (Outputs.size() < 4 || Operands.size() < 3)
    return {};

  std::string Result = "{\n";
  Result += "        uint64_t _nd_mops_dst = (uint64_t)(uintptr_t)(" +
            ExprFn(*Operands[0]) + ");\n";
  Result += "        uint64_t _nd_mops_src = (uint64_t)(uintptr_t)(" +
            ExprFn(*Operands[1]) + ");\n";
  Result += "        uint64_t _nd_mops_count = (uint64_t)(" +
            ExprFn(*Operands[2]) + ");\n";
  Result += "        uint64_t _nd_mops_nzcv;\n";
  Result += "        __asm__ volatile(\"cpyfp [%0]!, [%1]!, %2!\\n\\t"
            "mrs %3, nzcv\"\n";
  Result += "                         : \"+r\"(_nd_mops_dst), "
            "\"+r\"(_nd_mops_src),\n";
  Result += "                           \"+r\"(_nd_mops_count), "
            "\"=r\"(_nd_mops_nzcv)\n";
  Result += "                         :\n";
  Result += "                         : \"memory\", \"cc\");\n";
  const char *Names[] = {"_nd_mops_dst", "_nd_mops_src", "_nd_mops_count",
                         "_nd_mops_nzcv"};
  for (size_t I = 0; I < 4; ++I)
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
  case I::A64_MopsCpyFP:
    return renderMopsCopyPrologue(Outputs, Operands, ExprFn, VarFn, IsAlive);
  default:
    return {};
  }
}

std::string renderARMIntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   bool &HasCIntrinsics) {
  using I = Intrinsic;
  switch (Id) {
  case I::A64_Bfmmla: {
    if (Ops.size() < 3)
      return {};
    auto BitCastArg = [&](const char *Type, const std::string &Arg) {
      return std::string("__builtin_bit_cast(") + Type +
             ", (unsigned __int128)(" + Arg + "))";
    };
    HasCIntrinsics = true;
    return "__builtin_bit_cast(unsigned __int128, vbfmmlaq_f32(" +
           BitCastArg("float32x4_t", Ops[0]) + ", " +
           BitCastArg("bfloat16x8_t", Ops[1]) + ", " +
           BitCastArg("bfloat16x8_t", Ops[2]) + "))";
  }
  case I::A64_Frecpx: {
    if (Ops.size() < 2)
      return {};
    const char *RawTy = nullptr;
    const char *FloatTy = nullptr;
    const char *Builtin = nullptr;
    if (Ops[1] == "2") {
      RawTy = "uint16_t";
      FloatTy = "float16_t";
      Builtin = "vrecpxh_f16";
    } else if (Ops[1] == "4") {
      RawTy = "uint32_t";
      FloatTy = "float";
      Builtin = "vrecpxs_f32";
    } else if (Ops[1] == "8") {
      RawTy = "uint64_t";
      FloatTy = "double";
      Builtin = "vrecpxd_f64";
    } else {
      return {};
    }
    HasCIntrinsics = true;
    return std::string("__builtin_bit_cast(") + RawTy + ", " + Builtin +
           "(__builtin_bit_cast(" + FloatTy + ", (" + RawTy + ")(" + Ops[0] +
           "))))";
  }
  case I::A64_Fjcvtzs:
    if (Ops.empty())
      return {};
    return "__jcvt(__builtin_bit_cast(double, (uint64_t)(" + Ops[0] + ")))";
  case I::A64_Rcwcasp:
  case I::A64_Rcwcaspa:
  case I::A64_Rcwcaspal:
  case I::A64_Rcwcaspl:
  case I::A64_Rcwscasp:
  case I::A64_Rcwscaspa:
  case I::A64_Rcwscaspal:
  case I::A64_Rcwscaspl: {
    if (Ops.size() < 3)
      return {};
    const char *Builtin = intrinsicCName(Id);
    if (!Builtin)
      return {};
    return std::string(Builtin) + "((unsigned __int128)(" + Ops[0] +
           "), (unsigned __int128)(" + Ops[1] + "), (void *)(uintptr_t)(" +
           Ops[2] + "))";
  }
  case I::A64_Famax:
  case I::A64_Famin: {
    if (Ops.size() < 4)
      return {};
    const bool IsMax = Id == I::A64_Famax;
    const bool IsQ = Ops[2] == "16";
    const std::string &LaneBytes = Ops[3];
    const char *VecTy = nullptr;
    const char *Suffix = nullptr;
    if (LaneBytes == "2") {
      VecTy = IsQ ? "float16x8_t" : "float16x4_t";
      Suffix = "f16";
    } else if (LaneBytes == "4") {
      VecTy = IsQ ? "float32x4_t" : "float32x2_t";
      Suffix = "f32";
    } else if (LaneBytes == "8" && IsQ) {
      VecTy = "float64x2_t";
      Suffix = "f64";
    } else {
      return {};
    }
    const char *RawTy = IsQ ? "unsigned __int128" : "uint64_t";
    std::string Fn = IsMax ? "vamax" : "vamin";
    if (IsQ)
      Fn += "q";
    Fn += "_";
    Fn += Suffix;
    HasCIntrinsics = true;
    auto BitCastArg = [&](const std::string &Arg) {
      return std::string("__builtin_bit_cast(") + VecTy + ", (" + RawTy + ")(" +
             Arg + "))";
    };
    return std::string("__builtin_bit_cast(") + RawTy + ", " + Fn + "(" +
           BitCastArg(Ops[0]) + ", " + BitCastArg(Ops[1]) + "))";
  }
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
