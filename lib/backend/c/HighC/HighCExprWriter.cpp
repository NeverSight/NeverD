//===- HighCExprWriter.cpp - HighIR expression rendering --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// General expression rendering for the HighIR C emitter.  Binary operator
/// rendering and precedence handling live in HighCExprBinOp.cpp.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

std::string HighCWriter::varName(const MedVar &V) {
  if (CurrentFunc &&
      (CurrentFunc->FrameSize > 0 || CurrentFunc->FrameHeadroom > 0) &&
      V.Kind == MedVar::Reg &&
      V.RenameTag < 0 && V.SSAVer == 0 &&
      V.RegOff == getTargetRegInfo(Opts.TheArch).StackPointer)
    return "frame_base";
  if (V.RenameTag >= 0)
    return "v" + std::to_string(V.RenameTag);
  switch (V.Kind) {
  case MedVar::Stack:
    return "var_" + llvm::utohexstr(static_cast<uint64_t>(
                        V.StackOff < 0 ? -V.StackOff : V.StackOff));
  case MedVar::Param:
    return "arg" + std::to_string(V.Id);
  case MedVar::RetVal:
    return "retval";
  case MedVar::EHException:
    return "eh_exception";
  case MedVar::EHSelector:
    return "eh_selector";
  case MedVar::Temp:
    return "t" + std::to_string(V.Id) +
           (V.SSAVer == 0 ? "" : "_" + std::to_string(V.SSAVer));
  default:
    if (V.Id < 0)
      return "v_" + std::to_string(static_cast<unsigned>(-V.Id)) + "_" +
             std::to_string(V.SSAVer);
    return "v" + std::to_string(V.Id) + "_" + std::to_string(V.SSAVer);
  }
}

std::string HighCWriter::constStr(uint64_t Val) {
  if (Val == 0)
    return "0";
  if (Val <= limits::kDecimalConstThreshold)
    return std::to_string(Val);

  if (Val == 0xFFFFFFFF || Val == 0xFFFFFFFFFFFFFFFFULL)
    return "-1";

  int64_t SV = static_cast<int64_t>(Val);
  if (SV < 0 && SV >= -static_cast<int64_t>(limits::kDecimalConstThreshold))
    return std::to_string(SV);

  int32_t SV32 = static_cast<int32_t>(Val & 0xFFFFFFFF);
  if (Val <= 0xFFFFFFFF && SV32 < 0 &&
      SV32 >= -static_cast<int32_t>(limits::kDecimalConstThreshold))
    return std::to_string(SV32);

  return "0x" + llvm::utohexstr(Val);
}

std::string HighCWriter::renderUnaryOp(const HighExpr &E, int ParentPrec) {
  if (E.Operands.empty())
    return "/* bad unary */";

  switch (E.Op) {
  case NdOp::INT_NOT:
  case NdOp::INT_NEGATE:
    return "~" + exprStr(*E.Operands[0], 99);
  case NdOp::INT_NEG2:
    return "-" + exprStr(*E.Operands[0], 99);
  case NdOp::BOOL_NOT:
    return "!" + exprStr(*E.Operands[0], 99);
  case NdOp::INT_ZEXT: {
    auto &Inner = *E.Operands[0];
    if (Inner.Kind == ExprKind::Const && Inner.ConstVal == 0)
      return "0";
    if (Inner.Type && E.Type && Inner.Type->Size == E.Type->Size)
      return exprStr(Inner, ParentPrec);
    return "(" + typeToC(E.Type) + ")" + exprStr(Inner, 99);
  }
  case NdOp::INT_SEXT: {
    auto &Inner = *E.Operands[0];
    if (Inner.Kind == ExprKind::Const && Inner.ConstVal == 0)
      return "0";
    if (Inner.Type && E.Type && Inner.Type->Size == E.Type->Size)
      return exprStr(Inner, ParentPrec);
    return "(" + typeToC(E.Type) + ")(" +
           (Inner.Type ? typeToC(Inner.Type) : "uint32_t") + ")" +
           exprStr(Inner, 99);
  }
  case NdOp::FLOAT_TRUNC:
    return "(" + typeToC(E.Type) + ")" + exprStr(*E.Operands[0], 99);
  case NdOp::POPCOUNT:
    return "__builtin_popcountll(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::LZCOUNT:
    return "__builtin_clzll(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_NEG:
    return "-" + exprStr(*E.Operands[0], 99);
  case NdOp::FLOAT_ABS:
    return "__builtin_fabs(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_SQRT:
    return "__builtin_sqrt(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_CEIL:
    return "__builtin_ceil(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_FLOOR:
    return "__builtin_floor(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_ROUND:
    return "__builtin_round(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_ROUNDEVEN:
    return "__builtin_nearbyint(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_ISNAN:
    return "__builtin_isnan(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_FLOAT2FLOAT:
    return "(" + typeToC(E.Type) + ")" + exprStr(*E.Operands[0], 99);
  default:
    return "/* unary " + std::to_string(static_cast<int>(E.Op)) + " */ " +
           exprStr(*E.Operands[0]);
  }
}

std::string HighCWriter::renderCallExpr(const HighExpr &E) {
  std::string Name = E.CallTarget;
  if (Name.empty())
    Name = (kAutoFuncPrefix + llvm::utohexstr(E.CallAddr)).str();

  if (E.IntrinsicId != Intrinsic::None) {
    std::vector<std::string> OpStrs;
    for (auto &Op : E.Operands)
      if (Op)
        OpStrs.push_back(exprStr(*Op));

    using I = Intrinsic;
    if (E.IntrinsicId == I::A64_GetFPSR ||
        E.IntrinsicId == I::A64_SetFPSR)
      OpStrs.insert(OpStrs.begin(), "\"FPSR\"");
    if (E.IntrinsicId == I::A64_GetFPCR || E.IntrinsicId == I::A64_SetFPCR)
      OpStrs.insert(OpStrs.begin(), "\"FPCR\"");

    bool IsFixedIntToFP = E.IntrinsicId == I::A64_ScvtfFixed ||
                          E.IntrinsicId == I::A64_UcvtfFixed;
    bool IsFixedFPToInt = E.IntrinsicId == I::A64_FcvtzsFixed ||
                          E.IntrinsicId == I::A64_FcvtzuFixed;
    if ((IsFixedIntToFP || IsFixedFPToInt) && OpStrs.size() == 2) {
      uint16_t GPRBytes = 0;
      if (IsFixedIntToFP && !E.Operands.empty() && E.Operands[0] &&
          E.Operands[0]->Type)
        GPRBytes = E.Operands[0]->Type->Size;
      else if (IsFixedFPToInt && E.Type)
        GPRBytes = E.Type->Size;

      if (GPRBytes == 4 || GPRBytes == 8)
        OpStrs.push_back(GPRBytes == 8 ? "1" : "0");
    }

    auto Rendered = renderIntrinsicCall(E.IntrinsicId, Opts.TheArch, OpStrs,
                                        HasCIntrinsics);
    if (!Rendered.empty())
      return Rendered;
  }

  if (E.IntrinsicId == Intrinsic::None && !Name.empty() && Name[0] == '_')
    Name = Name.substr(1);

  std::string S = Name + "(";
  for (size_t I = 0; I < E.Operands.size(); ++I) {
    if (I > 0)
      S += ", ";
    S += exprStr(*E.Operands[I]);
  }
  S += ")";
  return S;
}

std::string HighCWriter::exprStr(const HighExpr &E, int ParentPrec) {
  static thread_local int Depth = 0;
  struct Guard {
    int &D;
    Guard(int &D_) : D(D_) { ++D; }
    ~Guard() { --D; }
  };
  Guard G(Depth);
  if (Depth > 200)
    return "(0 /* truncated: expr too deep */)";

  switch (E.Kind) {
  case ExprKind::Var:
    return varName(E.Var);
  case ExprKind::Const:
    return constStr(E.ConstVal);
  case ExprKind::Undef:
    return "((" + typeToC(E.Type) +
           ")0 /* caller-saved register clobbered by call: unknown */)";
  case ExprKind::BinOp:
    return renderBinOp(E, ParentPrec);
  case ExprKind::UnaryOp:
    return renderUnaryOp(E, ParentPrec);
  case ExprKind::Load: {
    if (E.Operands.empty())
      return "/* bad load */";
    std::string Addr = exprStr(*E.Operands[0]);
    if (E.MemoryOrdering == NdMemoryOrdering::None) {
      auto Fwd = Analysis.StoreFwd.find(Addr);
      if (Fwd != Analysis.StoreFwd.end())
        return Fwd->second;
    }
    return memoryLoadExpr(E.Type, Addr, E.MemoryOrdering);
  }
  case ExprKind::Store: {
    if (E.Operands.size() < 2)
      return "/* bad store */";
    std::string Addr = exprStr(*E.Operands[0]);
    std::string Val = exprStr(*E.Operands[1]);
    return memoryStoreExpr(E.Operands[1]->Type, Addr, Val, E.MemoryOrdering);
  }
  case ExprKind::Call:
    return renderCallExpr(E);
  case ExprKind::Cast: {
    if (E.Operands.empty())
      return "/* bad cast */";
    std::string Ty = E.CastTo ? typeToC(E.CastTo) : typeToC(E.Type);
    return "(" + Ty + ")" + exprStr(*E.Operands[0], 99);
  }
  case ExprKind::Addr: {
    if (E.Operands.empty())
      return "/* bad addr */";
    const HighExpr &Operand = *E.Operands[0];
    if (Operand.Kind == ExprKind::Load && !Operand.Operands.empty()) {
      return "(" + typeToC(Operand.Type) + " *)(uintptr_t)(" +
             exprStr(*Operand.Operands[0]) + ")";
    }
    return "&" + exprStr(*E.Operands[0], 99);
  }
  case ExprKind::Field:
    if (E.Operands.empty())
      return "/* bad field */";
    if (E.ConstVal != 0)
      return "(" + exprStr(*E.Operands[0]) + ").field_" +
             std::to_string(E.ConstVal);
    return exprStr(*E.Operands[0]);
  case ExprKind::Phi:
    return varName(E.Var);
  default:
    return "/* unknown expr */";
  }
}

std::string HighCWriter::unwrapCastVar(const HighExpr &E) {
  if (E.Kind == ExprKind::Var)
    return varName(E.Var);
  if (E.Kind == ExprKind::Load && !E.Operands.empty()) {
    std::string Addr = exprStr(*E.Operands[0]);
    auto Fwd = Analysis.StoreFwd.find(Addr);
    if (Fwd != Analysis.StoreFwd.end())
      return Fwd->second;
  }
  if (E.Kind == ExprKind::UnaryOp &&
      (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT) && !E.Operands.empty())
    return unwrapCastVar(*E.Operands[0]);
  if (E.Kind == ExprKind::Cast && !E.Operands.empty())
    return unwrapCastVar(*E.Operands[0]);
  return {};
}

std::string HighCWriter::collapseHiLo(const HighExpr &Expr) {
  auto Result = tryCollapseHiLo(
      Expr, HiLoPairs, [this](const HighExpr &E) { return unwrapCastVar(E); });
  if (!Result.Collapsed.empty()) {
    Analysis.DeadVars.insert(Result.DeadLo);
    Analysis.DeadVars.insert(Result.DeadHi);
    Analysis.DeadStmts.insert(static_cast<const HighStmt *>(Result.DeadStmt));
    HasCIntrinsics = true;
  }
  return Result.Collapsed;
}

std::string HighCWriter::formatReturnExpr(const HighExpr &Expr) {
  auto HiLo = collapseHiLo(Expr);
  if (!HiLo.empty())
    return HiLo;

  if (FuncReturnType && FuncReturnType->Kind == NdTypeKind::Float) {
    const HighExpr *Raw = &Expr;
    if (Raw->Kind == ExprKind::UnaryOp && Raw->Op == NdOp::INT_ZEXT &&
        !Raw->Operands.empty() && Raw->Operands[0] && Raw->Operands[0]->Type &&
        Raw->Operands[0]->Type->Size == FuncReturnType->Size)
      Raw = Raw->Operands[0].get();
    if (Raw->Type && Raw->Type->Kind == NdTypeKind::Int &&
        Raw->Type->Size == FuncReturnType->Size) {
      auto RawType = NdType::makeInt(FuncReturnType->Size, false);
      return "__builtin_bit_cast(" + typeToC(FuncReturnType) + ", (" +
             typeToC(RawType) + ")(" + exprStr(*Raw) + "))";
    }
  }

  if (!FuncReturnType || FuncReturnType->Kind != NdTypeKind::Int)
    return exprStr(Expr);

  if (Expr.Kind == ExprKind::UnaryOp &&
      (Expr.Op == NdOp::INT_ZEXT || Expr.Op == NdOp::INT_SEXT) &&
      !Expr.Operands.empty()) {
    auto &Inner = *Expr.Operands[0];
    if (Inner.Type && Inner.Type->Kind == NdTypeKind::Int) {
      if (Inner.Type->Size == FuncReturnType->Size)
        return exprStr(Inner);
      return "(" + typeToC(FuncReturnType) + ")" + exprStr(Inner, 99);
    }
  }

  if (Expr.Type && Expr.Type->Kind == NdTypeKind::Int &&
      Expr.Type->Size > FuncReturnType->Size) {
    return "(" + typeToC(FuncReturnType) + ")" + exprStr(Expr);
  }

  return exprStr(Expr);
}

} // namespace neverd
