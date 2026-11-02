//===- MedEntryStackOffsets.h - Scoped entry-stack queries ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse the entry-stack definition index within an unchanged function.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_PASS_MEDENTRYSTACKOFFSETS_H
#define NEVERD_IR_MED_PASS_MEDENTRYSTACKOFFSETS_H

#include "neverd/Common.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace neverd {

struct MedFunc;
struct MedVar;

/// Build lazily on the first fallback query. Each query has the same proof
/// state and work allowance as entryStackOffset(); only the definition index
/// and authenticated entry occurrences survive. Retention is bounded by the
/// function's definitions. Invalidate after any operation, PHI or entry change.
class MedEntryStackOffsets {
public:
  MedEntryStackOffsets(const MedFunc &Func, Arch Architecture,
                       BinaryFormat Format);
  ~MedEntryStackOffsets();
  MedEntryStackOffsets(const MedEntryStackOffsets &) = delete;
  MedEntryStackOffsets &operator=(const MedEntryStackOffsets &) = delete;

  std::optional<int64_t> query(const MedVar &Value);
  void invalidate();

private:
  struct Impl;
  const MedFunc &Func;
  Arch Architecture;
  BinaryFormat Format;
  std::unique_ptr<Impl> Index;
};

} // namespace neverd

#endif // NEVERD_IR_MED_PASS_MEDENTRYSTACKOFFSETS_H
