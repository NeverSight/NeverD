//===- X86RegistrationSEHStateStore.h - State stores ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds checked callback state transitions to their surviving MedIR stores.

#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONSEHSTATESTORE_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONSEHSTATESTORE_H

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {
struct MedFunc;
struct MedOp;
struct RegistrationSEHStateStore;

struct X86RegistrationSEHStateStoreBinding {
  const MedOp *Operation = nullptr;
  const RegistrationSEHStateStore *Source = nullptr;
  /// Sorted complete results, including the high bytes retained by a narrow
  /// store. The source transitions prove that this set covers every invocation.
  std::vector<int32_t> TargetLevels;
  /// Consecutive physical-write identities follow source occurrence order,
  /// then the sorted target levels within each occurrence.
  uint64_t FirstWriteID = 0;
};

/// Return callback stores in source occurrence order. Each checked transition
/// must preserve the untouched high bytes and produce a valid complete level.
/// The source solver owns address and transition semantics. Native projection
/// and replay retain their ordinary checks of the current memory operand.
/// IncludeParentStores also returns ordinary parent writes. Computed parent
/// values retain their LowIR result receipt and require independent native
/// value/address validation; callback writes still require literal bindings.
std::optional<std::vector<X86RegistrationSEHStateStoreBinding>>
getX86RegistrationSEHStateStores(const MedFunc &Function,
                                 bool IncludeParentStores = false);

/// Return sorted scopes selected only by callback writes, including enclosing
/// scopes. Every scope must be reachable from a parent state or callback write.
std::optional<std::vector<uint32_t>>
getX86RegistrationSEHRetainedScopes(const MedFunc &Function);

} // namespace neverd

#endif
