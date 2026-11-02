//===- LowScalarConstant.h - Exact integer constant results ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Evaluates fixed-width scalar LowIR operations from checked input facts.

#ifndef NEVERD_IR_LOW_LOWSCALARCONSTANT_H
#define NEVERD_IR_LOW_LOWSCALARCONSTANT_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SourceCallOccurrence.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>

namespace neverd {

struct CallRegisterBodyView;

/// Evaluate the bounded scalar operations shared by exact frame-value
/// propagation and image-address inventories. Results wrap at the declared
/// width; a pointer identity, a memory value or an unsupported operation is
/// never supplied by this query. The caller owns the reaching operand facts.
std::optional<uint64_t> lowScalarConstantResult(
    const LowOp &Operation,
    llvm::function_ref<std::optional<uint64_t>(const NdVar &)> ValueOf);

/// Result of one exact physical register query at an original call. An
/// unavailable value is not a zero. Budget exhaustion discards all facts.
struct LowCallRegisterConstant {
  std::optional<uint64_t> Value;
  bool OccurrenceFound = false;
  bool BudgetExceeded = false;
};

/// Uses the same bounded, instruction-local temporary and register-byte facts
/// as constantRegisterCallTargets. Only the requested call's block contributes
/// values; calls and opaque effects invalidate earlier register facts.
LowCallRegisterConstant
constantRegisterValueAtCall(const LowFunc &Function,
                            const SourceCallOccurrenceKey &Site,
                            const NdVar &Register);

/// Exact register-call values established locally or along a bounded unique
/// direct-predecessor chain. The keys retain the original indirect occurrences.
/// Joins, independent roots, exceptional edges and cycles provide no inherited
/// facts. Calls, opaque effects, loads and literal memory-call slots provide
/// none. These are numeric values, not function ownership or ABI evidence.
std::map<SourceCallOccurrenceKey, va_t>
constantRegisterCallTargets(const LowFunc &Function);

/// The same proof for retained call-summary bodies, with their coverage and
/// independent-root facts. A cache hit cannot change the target proof.
std::map<SourceCallOccurrenceKey, va_t>
constantRegisterCallTargets(const CallRegisterBodyView &Function);

/// Bitwise operations whose exact scalar operand absorbs the other input.
/// Reuses the call-value reader's byte facts, without predecessor or address
/// constants. Calls and opaque effects invalidate facts. Results borrow the
/// unchanged operations and consume a shared work allowance.
std::set<const LowOp *>
lowBlockAbsorbingConstants(llvm::ArrayRef<LowBlock> Blocks, size_t &Remaining);

} // namespace neverd

#endif
