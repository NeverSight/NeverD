//===- HighSourceMap.h - Non-owning source expression observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHSOURCEMAP_H
#define NEVERD_IR_HIGH_HIGHSOURCEMAP_H

#include "neverd/sigs/LibraryRecognition.h"

#include "llvm/ADT/DenseMap.h"

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
  StatementExpression,
  /// Complementary store identity when address simplification replaces the
  /// destination expression but preserves the stored value.
  StoreValueExpression,
  /// Navigation for an actual computation or call, including computations
  /// inlined before any statement was emitted. Never library-folding evidence.
  ComputationExpression
};

/// An expression produced from one concrete MedIR occurrence. Weak ownership
/// deliberately loses a mapping when a transformation replaces the expression.
/// Surviving expressions are checked again at emission; a pointer by itself
/// does not establish a source span or permission to fold a region.
struct HighSourceObservation {
  va_t Function = 0;
  sigs::LibraryOccurrence Occurrence;
  std::weak_ptr<const HighExpr> Expression;
  /// Stores keep the address expression's identity as well as their original
  /// instruction address: paired stores have distinct destination expressions
  /// even though their occurrences belong to the same machine instruction.
  HighSourceKind Kind = HighSourceKind::Expression;
  /// The original lowered statement kind. Sharing an address cannot authorize
  /// a changed or synthetic statement. A surviving call expression may drop
  /// its unused assignment while retaining its own occurrence.
  std::optional<StmtKind> StatementKind;
  /// Original ExprKind tag; keep the IR enum out of C dialect AST headers.
  std::optional<uint8_t> ExpressionKind;
};

using HighSourceMap = std::vector<HighSourceObservation>;

/// Observe an actual lowering event. Assignments, calls and stores keep weak
/// expression identity so discarding a primary result or emitting several
/// operations at one address cannot sever the surviving navigation anchor.
HighSourceObservation observeHighStatement(va_t Function,
                                           sigs::LibraryOccurrence Occurrence,
                                           const HighStmt &Statement);

/// Observe a newly lowered computation or call expression, excluding leaves.
/// A transport of an existing expression (COPY) is not a new computation.
std::optional<HighSourceObservation>
observeHighComputation(va_t Function, sigs::LibraryOccurrence Occurrence,
                       const std::shared_ptr<HighExpr> &Expression);

/// Shared statement navigation authority for HighIR text and emitted C.
/// An expression identity is stronger than an address/kind match; other
/// statements retain the existing unique address/kind contract. No ownership is
/// extended.
class HighStatementSourceIndex {
public:
  explicit HighStatementSourceIndex(const HighSourceMap &Sources);
  std::optional<sigs::LibraryOccurrence>
  lookup(va_t Function, const HighStmt &Statement) const;
  /// The surviving computation assigned or returned, optionally inside scalar
  /// views introduced while slicing a PHI. This identifies the computation,
  /// never the synthetic copy or its placement address.
  std::optional<sigs::LibraryOccurrence>
  lookupComputation(va_t Function, const HighStmt &Statement) const;

private:
  using Key = std::tuple<va_t, va_t, StmtKind>;
  std::map<Key, std::set<sigs::LibraryOccurrence>> Statements;
  std::set<std::pair<va_t, va_t>> ExpressionSites;
  std::map<std::tuple<va_t, va_t, const HighExpr *>,
           std::vector<HighSourceObservation>>
      Expressions;
  std::map<std::tuple<va_t, va_t, const HighExpr *>,
           std::vector<HighSourceObservation>>
      StoreValues;
  llvm::DenseMap<std::pair<va_t, const HighExpr *>,
                 std::vector<HighSourceObservation>>
      Computations;
};

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHSOURCEMAP_H
