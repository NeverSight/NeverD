//===- AArch64ReturnProtection.h - Source return protection -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_AARCH64_AARCH64RETURNPROTECTION_H
#define NEVERD_IR_MED_AARCH64_AARCH64RETURNPROTECTION_H

#include "neverd/ir/med/MedIR.h"

#include <optional>
#include <set>

namespace neverd {

/// A conventional entry-SP return-address signing policy. Other pointer
/// authentication, including recipes with a second modifier, stays explicit.
struct AArch64ReturnProtection {
  bool BKey = false;
  bool Leaf = false;
};

struct AArch64ReturnProtectionProjection {
  AArch64ReturnProtection Protection;
  /// Only these operations implement the proved policy. The machine IR is
  /// unchanged; the source converter omits them and carries the policy.
  std::set<const MedOp *> Operations;
};

std::optional<AArch64ReturnProtectionProjection>
findAArch64ReturnProtection(const MedFunc &Func, BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_IR_MED_AARCH64_AARCH64RETURNPROTECTION_H
