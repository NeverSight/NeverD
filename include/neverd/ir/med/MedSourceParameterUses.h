#ifndef NEVERD_IR_MED_MEDSOURCEPARAMETERUSES_H
#define NEVERD_IR_MED_MEDSOURCEPARAMETERUSES_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
struct MedFunc;
struct SourceFunctionTypeHint;

enum class SourceEntryDemand {
  EffectsAndReturns,
  // Positive evidence for inputs reaching effects. This cannot prove that a
  // return-only input is dead, or that any result carrier is fully defined.
  EffectsOnly
};

/// Observable incoming bytes keyed by physical entry register. Bit I denotes
/// byte I of that register, including wide vector carriers. Unknown graphs
/// return no proof; this does not establish types or a rewriting ABI.
std::optional<std::map<uint64_t, uint64_t>> observedMedSourceEntryBytes(
    const MedFunc &Function, const SourceFunctionTypeHint &Hint,
    SourceEntryDemand Demand = SourceEntryDemand::EffectsAndReturns);

/// Entry registers whose bytes can reach observable effects or a declared
/// source return carrier. Unknown/over-budget graphs return no proof. This
/// does not remove operations or establish a machine rewriting ABI.
std::optional<std::set<uint64_t>> observedMedSourceEntryRegisters(
    const MedFunc &Function, const SourceFunctionTypeHint &Hint,
    SourceEntryDemand Demand = SourceEntryDemand::EffectsAndReturns);

/// Identify complete pointer-sized inputs forwarded to source-bound pointer
/// parameters through COPY/PHI values, without conflicting scalar uses. The
/// result follows MedFunc::Params, including false entries for unused slots.
/// This refines source projection candidates only: it neither mutates MedIR
/// types nor establishes address provenance, ownership, or rewrite authority.
std::vector<bool> inferMedSourcePointerParameters(const MedFunc &Function);
} // namespace neverd
#endif
