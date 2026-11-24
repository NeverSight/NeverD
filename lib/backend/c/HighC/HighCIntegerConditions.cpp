//===- HighCIntegerConditions.cpp - Integer comparison folds --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognize compound integer conditions whose comparisons read the same
/// machine values. The ordinary comparison renderer owns their C types.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/backend/Limits.h"

#include <utility>

namespace neverd {
namespace {

bool integerBits(const TypeRef &Ty) {
  return Ty && Ty->Size &&
         (Ty->Kind == NdTypeKind::Int || Ty->Kind == NdTypeKind::Ptr);
}

bool literalZero(const HighExpr *E) {
  return E && E->Kind == ExprKind::Const && integerBits(E->Type) &&
         E->ConstVal == 0;
}

const HighExpr *otherOperandOfZero(const HighExpr &E) {
  if (literalZero(E.Operands[1].get()))
    return E.Operands[0].get();
  if (literalZero(E.Operands[0].get()))
    return E.Operands[1].get();
  return nullptr;
}

bool isBinary(const HighExpr *E, NdOp Op) {
  return E && E->Kind == ExprKind::BinOp && E->Op == Op &&
         E->Operands.size() == 2 && E->Operands[0] && E->Operands[1];
}

} // namespace

const HighExpr *HighCWriter::comparisonOperand(const HighExpr &E) const {
  const HighExpr *Value = &E;
  for (unsigned Depth = 0; Depth < limits::backend::kHighCComparisonValueDepth;
       ++Depth) {
    if (Value->Kind == ExprKind::Var || Value->Kind == ExprKind::Phi ||
        Value->Kind == ExprKind::Const)
      break;
    if (Value->Kind != ExprKind::BinOp || Value->Op == NdOp::SUBBYTES) {
      if (const HighExpr *Var = sameWidthVariable(*Value))
        return Var;
      break;
    }
    if (Value->Operands.size() != 2 || !integerBits(Value->Type))
      break;
    const HighExpr *Next = nullptr;
    switch (Value->Op) {
    case NdOp::INT_ADD:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      Next = otherOperandOfZero(*Value);
      break;
    case NdOp::INT_SUB:
      if (literalZero(Value->Operands[1].get()))
        Next = Value->Operands[0].get();
      break;
    default:
      return Value;
    }
    if (!Next || !integerBits(Next->Type) ||
        Next->Type->Size != Value->Type->Size)
      break;
    Value = Next;
  }
  return Value;
}

bool HighCWriter::sameComparisonValue(const HighExpr *A, const HighExpr *B,
                                      unsigned Depth) const {
  if (!A || !B || Depth >= limits::backend::kHighCComparisonValueDepth)
    return false;
  A = comparisonOperand(*A);
  B = comparisonOperand(*B);
  if (A->Kind != B->Kind || !integerBits(A->Type) || !integerBits(B->Type) ||
      A->Type->Size != B->Type->Size)
    return false;
  switch (A->Kind) {
  case ExprKind::Var:
  case ExprKind::Phi:
    return A->Var.Kind != MedVar::Unspecified && A->Var == B->Var &&
           A->Var.Size == B->Var.Size && A->Var.TheArch == B->Var.TheArch &&
           A->Var.RenameTag == B->Var.RenameTag;
  case ExprKind::Const:
    return A->structuralEq(*B);
  case ExprKind::Cast:
    // Signed and unsigned truncation keep the same low bits. Widening
    // conversions can produce different high bits.
    if (A->Operands.size() != 1 || B->Operands.size() != 1 || !A->Operands[0] ||
        !B->Operands[0] || !integerBits(A->Operands[0]->Type) ||
        A->Type->Size >= A->Operands[0]->Type->Size)
      return false;
    return sameComparisonValue(A->Operands[0].get(), B->Operands[0].get(),
                               Depth + 1);
  case ExprKind::Load:
    // Ordinary reads of the same address can be repeated in a condition made
    // only of stable operands. An ordered read or a call must keep every use.
    if (A->MemoryOrdering != NdMemoryOrdering::None ||
        B->MemoryOrdering != NdMemoryOrdering::None ||
        A->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        B->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        A->Operands.size() != 1 || B->Operands.size() != 1)
      return false;
    return sameComparisonValue(A->Operands[0].get(), B->Operands[0].get(),
                               Depth + 1);
  case ExprKind::BinOp:
    // Include base + offset addresses for ordinary structure members.
    if (A->Op != NdOp::INT_ADD || !isBinary(A, A->Op) || !isBinary(B, A->Op))
      return false;
    return sameComparisonValue(A->Operands[0].get(), B->Operands[0].get(),
                               Depth + 1) &&
           sameComparisonValue(A->Operands[1].get(), B->Operands[1].get(),
                               Depth + 1);
  default:
    return false;
  }
}

const HighExpr *HighCWriter::strictLessComparison(const HighExpr &E) const {
  if (!isBinary(&E, NdOp::BOOL_AND) && !isBinary(&E, NdOp::INT_AND))
    return nullptr;
  const HighExpr *Le = truthTestedValue(E.Operands[0].get());
  const HighExpr *Ne = truthTestedValue(E.Operands[1].get());
  if (isBinary(Le, NdOp::INT_NOTEQUAL))
    std::swap(Le, Ne);
  if ((!isBinary(Le, NdOp::INT_LESSEQUAL) &&
       !isBinary(Le, NdOp::INT_SLESSEQUAL)) ||
      !isBinary(Ne, NdOp::INT_NOTEQUAL))
    return nullptr;
  const auto &L = Le->Operands, &N = Ne->Operands;
  if (!integerBits(L[0]->Type) || !integerBits(L[1]->Type) ||
      L[0]->Type->Size != L[1]->Type->Size)
    return nullptr;
  return ((sameComparisonValue(L[0].get(), N[0].get()) &&
           sameComparisonValue(L[1].get(), N[1].get())) ||
          (sameComparisonValue(L[0].get(), N[1].get()) &&
           sameComparisonValue(L[1].get(), N[0].get())))
             ? Le
             : nullptr;
}

ExprPtr HighCWriter::nonPositiveComparison(const HighExpr &E) const {
  if (!isBinary(&E, NdOp::BOOL_OR) && !isBinary(&E, NdOp::INT_OR))
    return nullptr;
  const HighExpr *Eq = truthTestedValue(E.Operands[0].get());
  const HighExpr *Lt = truthTestedValue(E.Operands[1].get());
  if (!isBinary(Eq, NdOp::INT_EQUAL))
    std::swap(Eq, Lt);
  if (!isBinary(Eq, NdOp::INT_EQUAL))
    return nullptr;
  if (isBinary(Lt, NdOp::INT_NOTEQUAL))
    Lt = truthTestedValue(otherOperandOfZero(*Lt));
  if (!isBinary(Lt, NdOp::INT_SLESS) || !literalZero(Lt->Operands[1].get()) ||
      !sameComparisonValue(truthTestedValue(otherOperandOfZero(*Eq)),
                           truthTestedValue(Lt->Operands[0].get())) ||
      !readsSameBytes(E.Operands[0].get(), E.Operands[1].get()))
    return nullptr;
  const auto Bytes = zeroTestBytes(*Lt->Operands[0], *Lt->Operands[1]);
  return Bytes ? HighExpr::makeBinop(NdOp::INT_SLESSEQUAL, Lt->Operands[0],
                                     HighExpr::makeConst(0, Bytes))
               : nullptr;
}

} // namespace neverd
