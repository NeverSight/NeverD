//===- MedOpToExpr.cpp - MedOp to HighExpr expression tree mapping --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Maps each MedOp (NdOp opcode) to a HighExpr expression tree node.
/// This is the core of the MedIR → HighIR expression lowering.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

ExprPtr MedToHighConverter::medOpToExpr(const MedOp &Op) {
  switch (Op.Opcode) {
  case NdOp::COPY:
    if (Op.NumInputs >= 1)
      return medvarToExpr(Op.Inputs[0]);
    break;

  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL:
    if (Op.NumInputs >= 2) {
      if ((Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR) &&
          Op.Inputs[0] == Op.Inputs[1])
        return medvarToExpr(Op.Inputs[0]);
      return HighExpr::makeBinop(Op.Opcode, medvarToExpr(Op.Inputs[0]),
                                 medvarToExpr(Op.Inputs[1]));
    }
    break;

  case NdOp::INT_NOT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NEG2:
  case NdOp::BOOL_NOT:
  case NdOp::FLOAT_NEG:
  case NdOp::FLOAT_ABS:
  case NdOp::FLOAT_SQRT:
  case NdOp::FLOAT_CEIL:
  case NdOp::FLOAT_FLOOR:
  case NdOp::FLOAT_ROUND:
  case NdOp::FLOAT_ROUNDEVEN:
  case NdOp::FLOAT_ISNAN:
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_UINT2FLOAT:
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_FLOAT2UINT:
  case NdOp::FLOAT_TRUNC:
  case NdOp::FLOAT_FLOAT2FLOAT:
  case NdOp::LZCOUNT:
  case NdOp::POPCOUNT:
  case NdOp::CAST:
    if (Op.NumInputs >= 1)
      return HighExpr::makeUnary(Op.Opcode, medvarToExpr(Op.Inputs[0]));
    break;

  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT: {
    if (Op.NumInputs >= 1) {
      auto Expr = HighExpr::makeUnary(Op.Opcode, medvarToExpr(Op.Inputs[0]));
      Expr->Type = NdType::makeInt(Op.Output.Size);
      return Expr;
    }
    break;
  }

  case NdOp::LOAD: {
    if (Op.NumInputs >= 1) {
      auto &AddrVar = Op.Inputs[0];
      ExprPtr AddrExpr;
      if (AddrVar.Id >= 0) {
        auto AddrKey = varKey(AddrVar);
        auto DefIt = DefExpr.find(AddrKey);
        if (DefIt != DefExpr.end() && DefIt->second->Kind != ExprKind::Call)
          AddrExpr = DefIt->second;
      }
      return HighExpr::makeLoad(AddrExpr ? AddrExpr : medvarToExpr(AddrVar),
                                NdType::makeInt(Op.Output.Size),
                                Op.MemoryOrdering);
    }
    break;
  }

  case NdOp::SELECT:
    if (Op.NumInputs >= 3) {
      auto Expr = std::make_shared<HighExpr>();
      Expr->Kind = ExprKind::BinOp;
      Expr->Op = NdOp::SELECT;
      Expr->Operands.push_back(medvarToExpr(Op.Inputs[0]));
      Expr->Operands.push_back(medvarToExpr(Op.Inputs[1]));
      Expr->Operands.push_back(medvarToExpr(Op.Inputs[2]));
      Expr->Type = NdType::makeInt(Op.Output.Size);
      return Expr;
    }
    break;

  case NdOp::SUBBYTES:
    if (Op.NumInputs >= 2) {
      auto Expr = std::make_shared<HighExpr>();
      Expr->Kind = ExprKind::BinOp;
      Expr->Op = NdOp::SUBBYTES;
      Expr->Operands.push_back(medvarToExpr(Op.Inputs[0]));
      Expr->Operands.push_back(medvarToExpr(Op.Inputs[1]));
      Expr->Type = NdType::makeInt(Op.Output.Size);
      return Expr;
    }
    if (Op.NumInputs >= 1)
      return medvarToExpr(Op.Inputs[0]);
    break;

  case NdOp::CONCAT:
    if (Op.NumInputs >= 2) {
      auto Expr = std::make_shared<HighExpr>();
      Expr->Kind = ExprKind::BinOp;
      Expr->Op = NdOp::CONCAT;
      Expr->Operands.push_back(medvarToExpr(Op.Inputs[0]));
      Expr->Operands.push_back(medvarToExpr(Op.Inputs[1]));
      Expr->Type = NdType::makeInt(Op.Output.Size);
      return Expr;
    }
    break;

  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    if (Op.NumInputs >= 2)
      return HighExpr::makeBinop(Op.Opcode, medvarToExpr(Op.Inputs[0]),
                                 medvarToExpr(Op.Inputs[1]));
    break;

  case NdOp::CALL:
  case NdOp::INDIR_CALL: {
    va_t CallTarget = 0;
    if (Op.NumInputs >= 1 && Op.Inputs[0].isConst())
      CallTarget = Op.Inputs[0].ConstVal;
    std::string Name;
    if (FuncNames) {
      auto NameIt = FuncNames->find(CallTarget);
      Name = NameIt != FuncNames->end()
                 ? NameIt->second
                 : (kAutoFuncPrefix + llvm::utohexstr(CallTarget)).str();
    } else {
      Name = (kAutoFuncPrefix + llvm::utohexstr(CallTarget)).str();
    }
    return HighExpr::makeCall(Name, CallTarget, {});
  }

  case NdOp::INTRINSIC: {
    auto IID = intrinsicId(Op);
    std::vector<ExprPtr> Args;
    for (uint8_t I = 1; I < Op.NumInputs; ++I)
      Args.push_back(medvarToExpr(Op.Inputs[I]));
    auto Expr = HighExpr::makeCall(intrinsicName(IID), 0, std::move(Args));
    Expr->IntrinsicId = IID;
    if (Op.Output.Size > 0)
      Expr->Type = NdType::makeInt(Op.Output.Size, false);
    return Expr;
  }

  default:
    break;
  }
  return HighExpr::makeConst(0, 0);
}

} // namespace neverd
