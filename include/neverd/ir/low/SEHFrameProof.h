//===- SEHFrameProof.h - Local-unwind frame arguments -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Proves exact frame arguments over all ordinary LowIR predecessors.

#ifndef NEVERD_IR_LOW_SEHFRAMEPROOF_H
#define NEVERD_IR_LOW_SEHFRAMEPROOF_H

#include "neverd/ir/low/LowIR.h"

#include <map>
#include <string>
#include <utility>

namespace neverd {

struct TargetRegInfo;

struct LowSEHFrameProof {
  /// Exact entry-relative SP at direct calls whose first Win64 argument is
  /// that same value on every reaching ordinary path. No runtime identity,
  /// continuation ownership or unwind-prologue permission is implied.
  std::map<std::pair<va_t, int>, int64_t> Calls;
  std::string DependencyDigest;
  /// Complete decoded callees inspected to retain memory across a call.
  std::vector<va_t> CalleeDependencies;
};

/// Intersection analysis over ordinary CFG edges. Independent and exceptional
/// entries start without the function's SP identity. Unknown calls/stores
/// invalidate saved memory. Optional decoded leaf callees prove concrete
/// effects; no callee memory contract is inferred from an ABI.
/// Exhausted budgets and incomplete inputs return no proof.
LowSEHFrameProof
proveLowSEHFrames(const LowFunc &Function, const TargetRegInfo &TRI,
                  size_t &WorkRemaining,
                  const std::map<va_t, const LowFunc *> *Callees = nullptr);

/// Binds a proof to all operations, control boundaries, roots and CFG edges.
/// This is a freshness check, not a replacement for the producing analysis.
std::string lowSEHFrameDependencyDigest(const LowFunc &Function);

/// Empty if any dependency is missing, duplicated or does not name its body.
std::string
lowSEHFrameDependencyDigest(const LowFunc &Function,
                            llvm::ArrayRef<va_t> Dependencies,
                            const std::map<va_t, const LowFunc *> *Callees);

} // namespace neverd

#endif
