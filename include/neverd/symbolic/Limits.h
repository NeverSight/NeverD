//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_LIMITS_H
#define NEVERD_SYMBOLIC_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::symbolic {

// include/neverd/symbolic/SymBitwise.h
/// The largest arity a truth table is ever built at.
///
/// A table of t inputs is 2^t bits, so this is a statement about addressable
/// memory rather than about the algebra: past it the table itself stops
/// fitting, never mind anything computed from it.  It is deliberately far
/// above where any caller's own budget stops, so that it is a backstop against
/// an overflowed shift count rather than the limit anybody actually meets.
inline constexpr unsigned kMaxTruthTableAtoms = 30;

} // namespace neverd::symbolic

namespace neverd::limits::symbolic {

// include/neverd/symbolic/SymKnownBits.h
inline constexpr unsigned kSymKnownBitsMaxQueryWork = 256;
inline constexpr unsigned kSymKnownBitsMaxDepth = 32;
inline constexpr unsigned kSymKnownBitsMaxWidth = 128;
inline constexpr unsigned kSymKnownBitsMaxCachedFacts = 65536;

// include/neverd/symbolic/SymMBA.h
inline constexpr size_t kSymMBAMBARewriteCacheCapacity = 65536;

// lib/symbolic/expr/SymExprWalk.cpp and SymExprEval.cpp
// Small reachable regions dominate the bottom-up MBA walk. These tables grow
// normally when a graph needs more entries; this is not an analysis limit.
inline constexpr unsigned kSymExprTraversalInlineBuckets = 32;

// lib/symbolic/expr/SymExpr.cpp
/// Operand count at which block hashing replaces the short inline fold.
inline constexpr unsigned kSymExprBlockHashMinOperands = 8;

// include/neverd/symbolic/SymExpr.h
// Repeated small measurements keep their plans and native scratch inline.
// Larger plans grow normally; these capacities do not limit evaluation.
inline constexpr unsigned kSymEvalPlanInlineSteps = 16;
inline constexpr unsigned kSymEvalPlanInlineOperands =
    2 * kSymEvalPlanInlineSteps;

// lib/symbolic/expr/SymExprCompare.cpp
inline constexpr unsigned kSymExprCompareMaxNodes = 32;
inline constexpr unsigned kSymExprCompareMaxEdges = 64;
inline constexpr unsigned kSymExprCompareMaxTerms = 8;

// lib/symbolic/expr/SymExprLogic.cpp
inline constexpr unsigned kSymExprLogicRemoveDiscardedShiftTermsWork = 256;
inline constexpr unsigned kSymExprLogicSubsetWork = 256;

// lib/symbolic/expr/SymExprStruct.cpp
inline constexpr size_t kSymExprStructMaximum = 4096;

// Optional equality/range folds inspect bounded constant words and at most
// this many extension pairs. Exhaustion retains the exact remaining predicate.
inline constexpr unsigned kSymExprExtensionCompareMaxWords = 64;
inline constexpr unsigned kSymExprExtensionEqualityMaxSteps = 32;

// lib/symbolic/expr/SymExprStruct.cpp
inline constexpr int kSymExprStructConstantBitSliceDepthLimit = 32;
inline constexpr unsigned kSymExprStructSymContextConstantWindowRemaining = 256;
inline constexpr unsigned
    kSymExprStructSymContextConstantWindowAvailableMinArgument2 = 256U;

// lib/symbolic/expr/SymExprLogic.cpp
inline constexpr int kSymExprLogicRemoveDiscardedShiftTermsCNumOperandsALimit =
    8;
inline constexpr int kMaxDiscardedShiftWidth = 128;

} // namespace neverd::limits::symbolic

#endif // NEVERD_SYMBOLIC_LIMITS_H
