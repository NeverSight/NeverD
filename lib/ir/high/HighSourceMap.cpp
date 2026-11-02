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
ExprPtr statementExpression(const HighStmt &S) {
  const auto E = S.Kind == StmtKind::Assign ? S.Val
                 : S.Kind == StmtKind::Call ? S.CallExpr
                                            : nullptr;
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
          Expression                  ? HighSourceKind::StatementExpression
          : S.Kind == StmtKind::Store ? HighSourceKind::Store
                                      : HighSourceKind::Statement,
          S.Kind,
          Expression ? std::optional(static_cast<uint8_t>(Expression->Kind))
                     : std::nullopt};
}

HighStatementSourceIndex::HighStatementSourceIndex(
    const HighSourceMap &Sources) {
  for (const auto &S : Sources) {
    if (!S.StatementKind || S.Occurrence.Address == InvalidVA ||
        S.Occurrence.Sequence < 0)
      continue;
    if (S.Kind == HighSourceKind::StatementExpression) {
      ExpressionSites.emplace(S.Function, S.Occurrence.Address);
      if (const auto E = S.Expression.lock(); E)
        Expressions[{S.Function, S.Occurrence.Address, E.get()}].push_back(S);
    } else if (S.Kind == HighSourceKind::Statement ||
               S.Kind == HighSourceKind::Store)
      Statements[{S.Function, S.Occurrence.Address, *S.StatementKind}].insert(
          S.Occurrence);
  }
}

std::optional<sigs::LibraryOccurrence>
HighStatementSourceIndex::lookup(va_t Function, const HighStmt &S) const {
  if (S.IsPhiCopy)
    return {};
  if (const auto E = statementExpression(S)) {
    const auto At = Expressions.find({Function, S.Addr, E.get()});
    if (At != Expressions.end()) {
      std::set<sigs::LibraryOccurrence> Origins;
      for (const auto &O : At->second)
        if (O.Expression.lock() == E &&
            O.ExpressionKind == static_cast<uint8_t>(E->Kind) &&
            (*O.StatementKind == S.Kind ||
             (*O.StatementKind == StmtKind::Assign &&
              S.Kind == StmtKind::Call &&
              O.ExpressionKind == static_cast<uint8_t>(ExprKind::Call))))
          Origins.insert(O.Occurrence);
      if (Origins.size() == 1)
        return *Origins.begin();
      return {};
    }
    if (ExpressionSites.contains({Function, S.Addr}))
      return {};
  }
  const auto At = Statements.find({Function, S.Addr, S.Kind});
  if (At != Statements.end() && At->second.size() == 1)
    return *At->second.begin();
  return {};
}

} // namespace neverd
