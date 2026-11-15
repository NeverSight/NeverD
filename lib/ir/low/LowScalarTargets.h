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

/// Requested physical register slices at the entry of an original block.
/// The vectors in the result retain the query order and contain only finite
/// covers. A singleton is an exact numeric value, never pointer identity.
using LowScalarEntryQueries = std::map<const LowBlock *, std::vector<NdVar>>;
using LowScalarEntryValues =
    std::map<const LowBlock *,
             std::vector<std::pair<NdVar, std::vector<uint64_t>>>>;

/// Borrow the original-body summaries from CallRegisterEffects. Ordinary
/// ABI-based MayWrite summaries do not satisfy this contract.
struct LowScalarCallEffects {
  Arch Architecture;
  const std::map<va_t, uint32_t> &BodyMayWrite;
};

/// Display hypotheses have no finite-cover or preservation authority. Keeping
/// a numeric candidate across a call says only what entry condition to test;
/// the call may replace it. This separate type cannot stand in for exact facts.
struct LowScalarEntryCandidates {
  LowScalarEntryValues Entries;
};

/// Propose entry conditions for isolated byte-transform replay. Uses the same
/// converged CFG transfer, retaining numeric candidates across calls. Unknown
/// definitions, opaque instructions and independent roots still erase them.
/// Consumers must explicitly condition every result on the returned register
/// bytes. Never use these candidates for branches, call arguments or rewrites.
std::optional<LowScalarEntryCandidates>
conditionalScalarEntryCandidates(const LowFunc &Function,
                                 const LowScalarEntryQueries &Queries);

/// Reuse the finite fixed point for block-entry registers. All declared
/// ordinary, module and exceptional roots start unknown, regardless of block
/// order. Incomplete lifts and unresolved indirect edges supply no inherited
/// facts. Only an original-body call summary can retain a register slice;
/// unknown calls and opaque effects discard values, with no implicit ABI
/// preservation. Publish only after complete convergence. A null
/// work account requests the complete finite fixed point without a work cutoff.
std::optional<LowScalarEntryValues>
finiteScalarBlockEntryValues(const LowFunc &Function,
                             const LowScalarEntryQueries &Queries,
                             size_t *RemainingWork = nullptr,
                             const LowScalarCallEffects *Calls = nullptr);

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
