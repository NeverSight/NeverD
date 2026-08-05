//===- HighExpr.cpp - HighExpr factories and free helpers -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HighExpr static factory methods and free helper functions (intrinsicName
/// etc.).  Display/print logic lives in HighIRPrint.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// intrinsic helpers
//===----------------------------------------------------------------------===//

Intrinsic intrinsicId(const MedOp &Op) {
  if (Op.NumInputs < 1)
    return Intrinsic::None;
  auto &In = Op.Inputs[0];
  if (!In.isConst())
    return Intrinsic::None;
  return static_cast<Intrinsic>(In.ConstVal);
}

std::string intrinsicName(const MedOp &Op) {
  auto Id = intrinsicId(Op);
  if (Id == Intrinsic::None)
    return {};
  return intrinsicName(Id);
}

//===----------------------------------------------------------------------===//
// HighExpr factories
//===----------------------------------------------------------------------===//

ExprPtr HighExpr::makeVar(MedVar V, TypeRef Ty) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Var;
  E->Var = V;
  E->Type = Ty ? Ty : NdType::makeInt(V.Size);
  return E;
}

ExprPtr HighExpr::makeConst(uint64_t Val, uint16_t Size) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Const;
  E->ConstVal = Val;
  E->Type = NdType::makeInt(Size, false);
  return E;
}

ExprPtr HighExpr::makeUndef(uint16_t Size) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Undef;
  E->Type = NdType::makeInt(Size, false);
  return E;
}

ExprPtr HighExpr::makeBinop(NdOp Op, ExprPtr LHS, ExprPtr RHS) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::BinOp;
  E->Op = Op;
  E->Operands.push_back(LHS);
  E->Operands.push_back(RHS);
  if (LHS->Type)
    E->Type = LHS->Type;
  return E;
}

ExprPtr HighExpr::makeUnary(NdOp Op, ExprPtr Operand) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::UnaryOp;
  E->Op = Op;
  E->Operands.push_back(Operand);
  if (Operand->Type)
    E->Type = Operand->Type;
  return E;
}

ExprPtr HighExpr::makeLoad(ExprPtr Addr, TypeRef Ty) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Load;
  E->Operands.push_back(Addr);
  E->Type = Ty;
  return E;
}

ExprPtr HighExpr::makeCall(const std::string &Target, va_t Addr,
                           std::vector<ExprPtr> Args) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Call;
  E->CallTarget = Target;
  E->CallAddr = Addr;
  E->Operands = std::move(Args);
  return E;
}

} // namespace neverd
