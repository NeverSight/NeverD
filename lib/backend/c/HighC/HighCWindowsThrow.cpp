//===- HighCWindowsThrow.cpp - Windows C++ throw rendering ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain runtime arguments unless they prove the Windows rethrow spelling.
//===----------------------------------------------------------------------===//
#include "HighCWriter.h"

namespace neverd {
namespace {
bool isNullThrowArgument(const HighExpr *Value) {
  for (unsigned Depth = 0; Value && Depth != 8; ++Depth) {
    if (Value->Kind == ExprKind::Const)
      return Value->ConstVal == 0;
    if ((Value->Kind == ExprKind::Cast || Value->Kind == ExprKind::BitCast ||
         (Value->Kind == ExprKind::UnaryOp &&
          (Value->Op == NdOp::INT_ZEXT || Value->Op == NdOp::INT_SEXT))) &&
        Value->Operands.size() == 1) {
      Value = Value->Operands[0].get();
      continue;
    }
    return false;
  }
  return false;
}
} // namespace

void HighCWriter::writeCxxThrowExpr(const HighStmt &Stmt,
                                    const HighExpr &ThrowCall) {
  const bool NullObject = !ThrowCall.Operands.empty() &&
                          isNullThrowArgument(ThrowCall.Operands[0].get());
  const bool Rethrow = ThrowCall.Operands.size() == 2 && NullObject &&
                       isNullThrowArgument(ThrowCall.Operands[1].get());
  if (!Opts.StructuredExceptionSyntax || ThrowCall.Operands.empty() ||
      (NullObject && !Rethrow)) {
    OS << exprStr(ThrowCall);
    return;
  }
  OS << "throw";
  if (auto Printed = CxxThrowPrints.find(&Stmt);
      Printed != CxxThrowPrints.end()) {
    OS << " " << Printed->second.Type << "(";
    for (size_t I = 0; I < Printed->second.Args.size(); ++I) {
      if (I)
        OS << ", ";
      OS << exprStr(*Printed->second.Args[I]);
    }
    OS << ")";
    return;
  }
  if (!ThrowCall.Operands.empty() && ThrowCall.Operands[0] && !Rethrow)
    OS << " " << exprStr(*ThrowCall.Operands[0]);
}

} // namespace neverd
