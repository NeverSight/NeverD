//===- MedCallTarget.h - Cached callable provenance -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Observe a callable's original value through exact same-block spills. An
/// import observation identifies its original load; it never rereads an IAT.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLTARGET_H
#define NEVERD_IR_MED_MEDCALLTARGET_H

#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <map>
#include <optional>
#include <tuple>

namespace neverd {

struct MedCallTarget {
  va_t Address;
  bool ThroughSlot;
  /// Original input-block LOAD index, meaningful when ThroughSlot is true.
  std::size_t LoadIndex = 0;
};

/// Query SSA values before \p Before. Calls, uncertain aliases, partial writes
/// and ordered memory prevent a stored value from reaching a later reload.
std::optional<MedCallTarget>
resolveMedCallTarget(const MedBlock &Block, size_t Before, const MedVar &Value,
                     size_t *WorkRemaining = nullptr);

/// Give a pre-SSA block prefix private value versions before using the same
/// proof. Overlapping register writes and intervening calls kill old carriers.
/// The original operations and runtime target operand remain unchanged.
/// Observation indexes still refer to the original input block.
std::optional<MedCallTarget>
resolvePreSSAMedCallTarget(const MedBlock &Block, const MedVar &Value,
                           Arch Architecture, size_t *WorkRemaining = nullptr);

/// The same proof for successive queries of one block that only grows
/// between them: each query versions just the operations appended since the
/// previous one, instead of the whole prefix again. Every query must use the
/// same work budget; exhausting it ends the later queries too. Observation
/// indexes refer to the queried block, as for resolvePreSSAMedCallTarget.
class PreSSAMedCallTargets {
public:
  explicit PreSSAMedCallTargets(Arch Architecture)
      : Architecture(Architecture) {}

  std::optional<MedCallTarget>
  resolve(const MedBlock &Block, const MedVar &Value, size_t *WorkRemaining);

private:
  using Key = std::tuple<MedVar::VarKind, int, uint64_t, uint16_t>;

  MedVar read(MedVar Value);
  bool version(const MedOp &Original, size_t *WorkRemaining);

  Arch Architecture;
  std::map<Key, int> Versions;
  int Next = 1;
  /// The versioned copy of the block prefix seen so far.
  MedBlock Proof;
  bool Failed = false;
};

/// Adapt original machine operations to the same reaching-value proof before
/// call summaries run. Keys are original block operation indexes. Neither
/// the machine operand nor an observed import value is replaced by this query.
std::map<size_t, MedCallTarget>
resolveLowBlockCallTargets(const LowBlock &Block, Arch Architecture,
                           size_t *WorkRemaining = nullptr);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLTARGET_H
