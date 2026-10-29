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
#include <map>
#include <optional>
#include <vector>

namespace neverd {

/// Complete overapproximations of INDIR_BR operands in the supplied CFG.
/// Missing values are unknown, never zero; incomplete work publishes nothing.
/// The caller owns code-target validation and replays after adding any edges.
std::optional<std::map<va_t, std::vector<va_t>>>
finiteScalarBranchTargets(const LowFunc &Function, size_t &RemainingWork);

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_LOWSCALARTARGETS_H
