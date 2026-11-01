//===- LowScalarTargets.h - Finite scalar targets ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded all-path scalar value sets for computed code branches.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_LOW_LOWSCALARTARGETS_H
#define NEVERD_LIB_IR_LOW_LOWSCALARTARGETS_H

#include "neverd/ir/low/LowIR.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {

struct CallRegisterBodyView;

/// Queries borrow operation identities from one immutable body. Each operand
/// receives a complete finite cover, or no entry when its value is unknown.
using LowScalarOperandQueries = std::map<const LowOp *, std::vector<unsigned>>;
using LowScalarOperandValues =
    std::map<std::pair<const LowOp *, unsigned>, std::vector<uint64_t>>;

/// The same finite dataflow as computed branches, for exact operand
/// occurrences. Joins union all paths, memory and opaque effects supply no
/// values, and budget exhaustion publishes nothing. Independent entry roots
/// in the summary projection provide no inherited register facts.
std::optional<LowScalarOperandValues>
finiteScalarOperandValues(CallRegisterBodyView Function,
                          const LowScalarOperandQueries &Queries,
                          size_t &RemainingWork);

/// Complete overapproximations of INDIR_BR operands in the supplied CFG.
/// Missing values are unknown, never zero; incomplete work publishes nothing.
/// The caller owns code-target validation and replays after adding any edges.
std::optional<std::map<va_t, std::vector<va_t>>>
finiteScalarBranchTargets(const LowFunc &Function, size_t &RemainingWork);

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_LOWSCALARTARGETS_H
