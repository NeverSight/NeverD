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
  /// Enclosing try level, or -1 (EH3) / -2 (EH4) directly under the frame.
  int32_t EnclosingLevel = -1;
  /// Filter expression address; zero marks a `__finally` (termination) scope.
  va_t FilterVA = 0;
  /// `__except` body, or `__finally` body when `FilterVA` is zero.
  va_t HandlerVA = 0;
  bool IsFinally = false;
};

/// One immediate store into the frame's try-level slot. Narrow stores retain
/// their unsigned literal bits; only the shared state analysis can prove the
/// complete level after preserving the untouched bytes.
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
  /// Signed full-word level, or the zero-extended immediate of a narrow store.
  int32_t Level = 0;
  uint8_t Width = 4;
};

/// A checked realigned local frame, distinct from the entry's EBP frame.
/// The Windows C++ runtime establishes EBP just beyond the registration node.
/// BaseOffset expresses ESI in that runtime coordinate, not in entry EBP.
struct RegistrationRealignedFrame {
  /// Architectural x86 register encoding; the checked layout currently uses
  /// ESI.
  uint8_t BaseRegister = 6;
  va_t DefinitionVA = 0;
  uint32_t Alignment = 0;
  uint32_t AllocationBytes = 0;
  int32_t BaseOffset = 0;
  int32_t SavedParentFrameOffset = 0;
  bool operator==(const RegistrationRealignedFrame &) const = default;
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
  /// Present when the local frame and runtime establisher have different
  /// coordinates from entry EBP. Existing EBP consumers must not use its
  /// offsets without an independent transfer proof for this anchor.
  std::optional<RegistrationRealignedFrame> RealignedFrame;

  /// C++ runtime EBP is immediately above its three-word registration node.
  /// Fixed LLVM frames save EBX/EDI/ESI above that node, so runtime EBP is
  /// twelve bytes below source EBP. The loader authenticates each layout;
  /// consumers must additionally require the appropriate language and proof.
  std::optional<int32_t> cxxRuntimeFrameOffset() const {
    if (SeededTryLevel != -1 ||
        (RegistrationOffset != -12 && RegistrationOffset != -24) ||
        TryLevelOffset != *RegistrationOffset + 8 ||
        (RealignedFrame && RegistrationOffset != -12))
      return std::nullopt;
    return *RegistrationOffset + 12;
  }

  /// Translate a native C++ runtime displacement into the source coordinate.
  std::optional<int32_t> cxxSourceFrameOffset(int32_t Offset) const {
    const auto Bias = cxxRuntimeFrameOffset();
    if (!Bias || int64_t(Offset) + *Bias < INT32_MIN)
      return std::nullopt;
    return Offset + *Bias;
  }

  bool hasCxxCallbackStack() const {
    return RealignedFrame || cxxRuntimeFrameOffset().value_or(0) != 0;
  }

  /// An absolute code-pointer field owned by this decoded SEH table. These
  /// references are runtime dispatch entries, not ordinary address-taken CFG
  /// roots. Independent references to the same code keep their own role.
  std::optional<va_t> scopePointerTarget(va_t Slot) const {
    if (!ScopeTableVA || (SeededTryLevel != -1 && SeededTryLevel != -2))
      return std::nullopt;
    const uint64_t Header = SeededTryLevel == -2 ? 16 : 0;
    if (ScopeTableVA > InvalidVA - Header || Slot < ScopeTableVA + Header)
      return std::nullopt;
    const uint64_t Offset = Slot - ScopeTableVA - Header;
    const uint64_t Index = Offset / 12;
    if (Index >= Scopes.size())
      return std::nullopt;
    const auto &Scope = Scopes[Index];
    if (Offset % 12 == 4 && Scope.FilterVA)
      return Scope.FilterVA;
    if (Offset % 12 == 8 && Scope.HandlerVA)
      return Scope.HandlerVA;
    return std::nullopt;
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHREGISTRATION_H
