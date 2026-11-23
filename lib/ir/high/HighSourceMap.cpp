//===- HighSourceMap.cpp - Surviving statement navigation identity --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared instruction observations for HighIR text and C navigation.
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/HighSourceMap.h"

#include "neverd/ir/high/HighIR.h"

namespace neverd {
namespace {
bool conditionalStatement(StmtKind Kind) {
  return Kind == StmtKind::If || Kind == StmtKind::IfElse ||
         Kind == StmtKind::While || Kind == StmtKind::DoWhile;
}

ExprPtr statementExpression(const HighStmt &S) {
  auto E = S.Kind == StmtKind::Assign     ? S.Val
           : S.Kind == StmtKind::Call     ? S.CallExpr
           : S.Kind == StmtKind::Store    ? S.StoreAddr
           : conditionalStatement(S.Kind) ? S.Cond
                                          : nullptr;
  // Structuring can reverse a branch or turn it into a loop. The surviving
  // predicate still belongs to the original conditional branch.
  while (conditionalStatement(S.Kind) && E && E->Kind == ExprKind::UnaryOp &&
         E->Op == NdOp::BOOL_NOT && E->Operands.size() == 1)
    E = E->Operands[0];
  return E;
}
} // namespace

HighSourceObservation observeHighStatement(va_t Function,
                                           sigs::LibraryOccurrence Occurrence,
                                           const HighStmt &S) {
  const auto Expression = statementExpression(S);
  return {Function,
          Occurrence,
          Expression,
          S.Kind == StmtKind::Store      ? HighSourceKind::Store
          : conditionalStatement(S.Kind) ? HighSourceKind::Statement
          : Expression                   ? HighSourceKind::StatementExpression
                                         : HighSourceKind::Statement,
          S.Kind,
          Expression ? std::optional(static_cast<uint8_t>(Expression->Kind))
                     : std::nullopt};
}

std::optional<HighSourceObservation>
observeHighComputation(va_t Function, sigs::LibraryOccurrence Occurrence,
                       const ExprPtr &Expression) {
  if (!Expression)
    return {};
  switch (Expression->Kind) {
  case ExprKind::BinOp:
  case ExprKind::UnaryOp:
  case ExprKind::Cast:
  case ExprKind::BitCast:
  case ExprKind::Call:
    return HighSourceObservation{
        Function,   Occurrence,
        Expression, HighSourceKind::ComputationExpression,
        {},         static_cast<uint8_t>(Expression->Kind)};
  default:
    return {};
  }
}

HighStatementSourceIndex::HighStatementSourceIndex(
    const HighSourceMap &Sources) {
  for (const auto &S : Sources) {
    if (S.Occurrence.Address == InvalidVA || S.Occurrence.Sequence < 0)
      continue;
    if (S.Kind == HighSourceKind::ComputationExpression) {
      if (const auto E = S.Expression.lock())
        Computations[{S.Function, E.get()}].push_back(S);
      continue;
    }
    if (!S.StatementKind)
      continue;
    if (S.Kind == HighSourceKind::StoreValueExpression) {
      if (const auto E = S.Expression.lock())
        StoreValues[{S.Function, S.Occurrence.Address, E.get()}].push_back(S);
      continue;
    }
    if (S.Kind == HighSourceKind::StatementExpression ||
        ((S.Kind == HighSourceKind::Store ||
          S.Kind == HighSourceKind::Statement) &&
         S.ExpressionKind)) {
      if (S.Kind == HighSourceKind::StatementExpression)
        ExpressionSites.emplace(S.Function, S.Occurrence.Address);
      if (const auto E = S.Expression.lock(); E)
        Expressions[{S.Function, S.Occurrence.Address, E.get()}].push_back(S);
    }
    if (S.Kind == HighSourceKind::Statement || S.Kind == HighSourceKind::Store)
      Statements[{S.Function, S.Occurrence.Address, *S.StatementKind}].insert(
          S.Occurrence);
  }
}

std::optional<sigs::LibraryOccurrence>
HighStatementSourceIndex::lookup(va_t Function, const HighStmt &S) const {
  if (S.IsPhiCopy)
    return {};
  const va_t Address = S.sourceAddress();
  if (S.Kind == StmtKind::Store) {
    auto Origins = [&](const auto &Index, const ExprPtr &E) {
      std::set<sigs::LibraryOccurrence> Result;
      if (!E)
        return Result;
      if (const auto At = Index.find({Function, Address, E.get()});
          At != Index.end())
        for (const auto &O : At->second)
          if (O.Expression.lock() == E && O.StatementKind == S.Kind &&
              O.ExpressionKind == static_cast<uint8_t>(E->Kind))
            Result.insert(O.Occurrence);
      return Result;
    };
    auto AddressOrigins = Origins(Expressions, S.StoreAddr);
    const auto ValueOrigins = Origins(StoreValues, S.StoreVal);
    if (AddressOrigins.empty())
      AddressOrigins = ValueOrigins;
    else if (!ValueOrigins.empty())
      std::erase_if(AddressOrigins,
                    [&](const auto &O) { return !ValueOrigins.contains(O); });
    if (AddressOrigins.size() == 1)
      return *AddressOrigins.begin();
    if (!ValueOrigins.empty())
      return {};
  }
  if (const auto E = statementExpression(S)) {
    const auto At = Expressions.find({Function, Address, E.get()});
    if (At != Expressions.end()) {
      std::set<sigs::LibraryOccurrence> Origins;
      for (const auto &O : At->second)
        if (O.Expression.lock() == E &&
            O.ExpressionKind == static_cast<uint8_t>(E->Kind) &&
            (*O.StatementKind == S.Kind ||
             (*O.StatementKind == StmtKind::If &&
              conditionalStatement(S.Kind)) ||
             (*O.StatementKind == StmtKind::Assign &&
              S.Kind == StmtKind::Call &&
              O.ExpressionKind == static_cast<uint8_t>(ExprKind::Call))))
          Origins.insert(O.Occurrence);
      if (Origins.size() == 1)
        return *Origins.begin();
      return {};
    }
    if (S.Kind != StmtKind::Store && !conditionalStatement(S.Kind) &&
        ExpressionSites.contains({Function, Address}))
      return {};
  }
  const auto At = Statements.find({Function, Address, S.Kind});
  if (At != Statements.end() && At->second.size() == 1)
    return *At->second.begin();
  return {};
}

std::optional<sigs::LibraryOccurrence>
HighStatementSourceIndex::lookupComputation(va_t Function,
                                            const HighStmt &S) const {
  auto E = S.Kind == StmtKind::Assign     ? S.Val
           : S.Kind == StmtKind::Return   ? S.RetVal
           : S.Kind == StmtKind::Call     ? S.CallExpr
           : conditionalStatement(S.Kind) ? S.Cond
                                          : nullptr;
  while (E) {
    if (const auto At = Computations.find({Function, E.get()});
        At != Computations.end()) {
      std::set<sigs::LibraryOccurrence> Origins;
      for (const auto &O : At->second)
        if (O.Expression.lock() == E &&
            O.ExpressionKind == static_cast<uint8_t>(E->Kind))
          Origins.insert(O.Occurrence);
      return Origins.size() == 1 ? std::optional(*Origins.begin())
                                 : std::nullopt;
    }
    // A PHI's byte view has no machine instruction of its own. Its printed
    // value can still contain the original computation. Do not search through
    // other arithmetic or follow variable definitions to invent an origin.
    const bool UnaryView =
        E->Operands.size() == 1 &&
        (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
         (E->Kind == ExprKind::UnaryOp &&
          (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)));
    const bool Slice = E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
                       E->Operands.size() == 2 && E->Operands[1] &&
                       E->Operands[1]->Kind == ExprKind::Const;
    if (!UnaryView && !Slice)
      break;
    E = E->Operands[0];
  }
  return {};
}

} // namespace neverd
