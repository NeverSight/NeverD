//===- LanguageEHRegistration.h - x86-32 registration chain ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized `_except_handler3`/`_except_handler4` registration records: the
/// flat scope table indexed by try level, the stores that say where each level
/// is current, and the prologue state that roots the `FS:[0]` chain.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHREGISTRATION_H
#define NEVERD_LOADER_LANGUAGEEHREGISTRATION_H

#include "neverd/Common.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// One `_except_handler3`/`_except_handler4` scope-table entry.  The scope
/// table is a flat array indexed by "try level"; nesting is expressed by each
/// entry naming its enclosing level rather than by containment of ranges,
/// which is why this model keeps the level graph instead of address ranges.
struct RegistrationScopeRecord {
  /// Enclosing try level, or -1 for a scope directly under the frame.
  int32_t EnclosingLevel = -1;
  /// Filter expression address; zero marks a `__finally` (termination) scope.
  va_t FilterVA = 0;
  /// `__except` body, or `__finally` body when `FilterVA` is zero.
  va_t HandlerVA = 0;
  bool IsFinally = false;
};

/// One store of a literal try level into the frame's try-level slot.
///
/// The scope table says which scope a level names but nothing about where that
/// level is current: the runtime reads the level out of the frame, so only the
/// stores the code makes say which region each scope guards.  Recovering them
/// is what turns the flat table back into address ranges.
struct RegistrationTryLevelStore {
  /// Address of the storing instruction.  The new level takes effect after it,
  /// so \c EndVA and not this is where the guarded region begins.
  va_t StoreVA = 0;
  /// Address just past the store.
  va_t EndVA = 0;
  int32_t Level = 0;
};

/// The prologue-established registration record for one x86-32 function.
struct RegistrationChainInfo {
  /// Address of the handler the prologue installed (`_except_handler3`,
  /// `_except_handler4`, `__CxxFrameHandler`, or a language runtime's own).
  va_t HandlerVA = 0;
  /// Address of the scope table or `FuncInfo` the prologue pushed.
  va_t ScopeTableVA = 0;
  /// Frame offset of the current-try-level slot, relative to the established
  /// frame register, when the function's own stores proved it.
  std::optional<int32_t> TryLevelOffset;
  /// The stores into that slot, in address order.  Empty when the slot could
  /// not be proven, which leaves the scopes without recovered ranges rather
  /// than giving them invented ones.
  std::vector<RegistrationTryLevelStore> TryLevelStores;
  /// The level the prologue seeded: -1 for `_except_handler3` and -2 for
  /// `_except_handler4`.  Both mean "no scope is current".
  std::optional<int32_t> SeededTryLevel;
  /// Address at which the prologue stored the new registration record, i.e.
  /// the value written to `FS:[0]`, expressed as a frame offset.
  std::optional<int32_t> RegistrationOffset;
  /// `_except_handler4` cookie fields.  Present only for the EH4 scope-table
  /// header, which precedes the entry array at a negative displacement.
  bool HasSecurityCookies = false;
  int32_t GSCookieOffset = 0;
  int32_t GSCookieXOROffset = 0;
  int32_t EHCookieOffset = 0;
  int32_t EHCookieXOROffset = 0;
  /// Native scope-table magic for `_except_handler4` (`0xFFFFFFFE` and the
  /// obfuscated variants), retained because it selects the header layout.
  uint32_t ScopeTableMagic = 0;
  std::vector<RegistrationScopeRecord> Scopes;
  /// Addresses at which the prologue/epilogue manipulate the chain.  These
  /// bound the region in which the registration record is live.
  va_t ChainInstallVA = 0;
  va_t ChainRemoveVA = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHREGISTRATION_H
