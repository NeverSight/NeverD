//===- GuardControlFlow.h - Validated indirect-call targets ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Session-owned CFG policy. The PE loader owns metadata validation; the
/// execution session owns check/dispatch ABI handling and export identities.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_GUARDCONTROLFLOW_H
#define NEVERD_EMULATION_GUARDCONTROLFLOW_H

#include "llvm/Support/Error.h"

#include <cstdint>
#include <set>

namespace neverd::emulation {
struct DriverImage;

namespace guard {
#define NEVERD_GUARD_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "GuardControlFlow.def"
#undef NEVERD_GUARD_VALUE
} // namespace guard

/// Exact declared entry points form the supported CFG profile. The profile
/// deliberately does not model Windows bitmap rounding into adjacent bytes.
/// A call to an undeclared address therefore stops explicitly.
class GuardControlFlow {
public:
  explicit GuardControlFlow(const DriverImage &Image);

  /// Register only an address assigned by the authoritative export registry.
  /// Framework table thunks and dynamically resolved exports use the same path.
  llvm::Error registerExportTarget(uint64_t Address);
  llvm::Error validateTarget(uint64_t Address) const;

private:
  bool Enabled = false;
  std::set<uint64_t> Targets;
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_GUARDCONTROLFLOW_H
