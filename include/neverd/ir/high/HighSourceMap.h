//===- HighSourceMap.h - Non-owning source expression observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHSOURCEMAP_H
#define NEVERD_IR_HIGH_HIGHSOURCEMAP_H

#include "neverd/sigs/LibraryRecognition.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

struct HighExpr;
struct HighStmt;
enum class StmtKind : uint8_t;
enum class HighSourceKind {
  Expression,
  Store,
  Statement,
  CallExpression,
  StatementExpression
};

/// An expression produced from one concrete MedIR occurrence. Weak ownership
/// deliberately loses a mapping when a transformation replaces the expression.
/// Surviving expressions are checked again at emission; a pointer by itself
/// does not establish a source span or permission to fold a region.
struct HighSourceObservation {
  va_t Function = 0;
  sigs::LibraryOccurrence Occurrence;
  std::weak_ptr<const HighExpr> Expression;
  /// Stores retain their original HighStmt address across structuring. More
  /// than one store occurrence at that address makes the mapping ambiguous.
  HighSourceKind Kind = HighSourceKind::Expression;
  /// The original lowered statement kind. Sharing an address cannot authorize
  /// a changed or synthetic statement. A surviving call expression may drop
  /// its unused assignment while retaining its own occurrence.
  std::optional<StmtKind> StatementKind;
  /// Original ExprKind tag; keep the IR enum out of C dialect AST headers.
  std::optional<uint8_t> ExpressionKind;
};

using HighSourceMap = std::vector<HighSourceObservation>;

/// Observe an actual lowering event. Assignments and calls keep weak expression
/// identity so discarding a primary result or emitting several operations at
/// one instruction address cannot sever the surviving call's navigation anchor.
HighSourceObservation observeHighStatement(va_t Function,
                                           sigs::LibraryOccurrence Occurrence,
                                           const HighStmt &Statement);

/// Shared statement navigation authority for HighIR text and emitted C.
/// An expression identity is stronger than an address/kind match; other
/// statements retain the existing unique address/kind contract. No ownership is
/// extended.
class HighStatementSourceIndex {
public:
  explicit HighStatementSourceIndex(const HighSourceMap &Sources);
  std::optional<sigs::LibraryOccurrence>
  lookup(va_t Function, const HighStmt &Statement) const;

private:
  using Key = std::tuple<va_t, va_t, StmtKind>;
  std::map<Key, std::set<sigs::LibraryOccurrence>> Statements;
  std::set<std::pair<va_t, va_t>> ExpressionSites;
  std::map<std::tuple<va_t, va_t, const HighExpr *>,
           std::vector<HighSourceObservation>>
      Expressions;
};

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHSOURCEMAP_H
