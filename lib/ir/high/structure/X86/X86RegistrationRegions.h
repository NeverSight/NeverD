//===- X86RegistrationRegions.h - x86 scopes as C regions -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The statements of an x86 `_except_handler3` or `_except_handler4` frame, put
/// into the `__try` of each scope.  Control-flow structuring knows nothing of
/// scopes, and /O2 lays a scope's code out in pieces with handlers between
/// them, but each scope is one `__try` in the source and each instruction's try
/// level says which: what jumps between pieces stays a jump.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_HIGH_STRUCTURE_X86_X86REGISTRATIONREGIONS_H
#define NEVERD_LIB_IR_HIGH_STRUCTURE_X86_X86REGISTRATIONREGIONS_H

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/ExceptionFunction.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

struct RegistrationRegionFacts {
  const std::vector<RegistrationScopeRecord> &Scopes;
  const RegistrationDisplayFacts &Facts;
  /// The scope of each handler and filter.
  const std::map<va_t, size_t> &ScopeOfRoot;
  ExceptionParseStatus ParseStatus;
  /// The value of each filter one expression can say.
  const std::map<va_t, ExprPtr> &FilterValues;
  /// The handlers of the scopes that print as blocks, with what their clauses
  /// hold beside them: a jump can enter a block.
  const std::set<va_t> &Blocks;
  /// Whether a statement calls a `__finally` body that never comes back.
  const std::function<bool(const HighStmt &)> &NeverComesBack;
};

/// \p Stmts with each scope's statements in one `SEHTry` and its handler's in
/// that try's clause.  Every path between statements that are no longer next to
/// each other is a jump.  Each try's address is its first statement's.
std::vector<HighStmt>
placeRegistrationRegions(std::vector<HighStmt> Stmts,
                         const RegistrationRegionFacts &In);

/// Remove the jumps to what runs next anyway, past the end of a list and of its
/// try: leaving a `__try` by its end runs a `__finally` as a normal
/// termination. An abnormal exit stays, unless it leaves by a `return` right
/// after it.
void dropNaturalJumps(std::vector<HighStmt> &Stmts);

/// Point each jump past the jumps it lands on, and make a jump to a `return` of
/// nothing to be read that `return`.
void threadJumps(std::vector<HighStmt> &Stmts);

/// When later cleanup leaves several statements at one address, put the label C
/// prints where every jump to it can reach, out of the `__try`s and clauses it
/// is not in.
void keepRegistrationLabelsInReach(std::vector<HighStmt> &Stmts);

/// The handler of a try a jump in \p Stmts enters from outside.  C has none.
std::optional<va_t> registrationTryEntered(const std::vector<HighStmt> &Stmts);

} // namespace neverd

#endif // NEVERD_LIB_IR_HIGH_STRUCTURE_X86_X86REGISTRATIONREGIONS_H
