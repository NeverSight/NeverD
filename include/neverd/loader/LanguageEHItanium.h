//===- LanguageEHItanium.h - Itanium language-specific data ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized `.gcc_except_table` records: the call-site table that names a
/// protected region's landing pad, the action chain that says what the pad
/// does, and the type table and exception-specification lists the actions
/// select from.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHITANIUM_H
#define NEVERD_LOADER_LANGUAGEEHITANIUM_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// One entry of the LSDA action chain.  A positive filter selects the
/// 1-based type-table entry a catch matches, zero is a cleanup that always
/// runs, and a negative filter selects a 1-based exception-specification list.
struct ItaniumAction {
  /// Byte offset of this action record inside the action table, which is how
  /// call sites and chained actions name it.
  uint64_t TableOffset = 0;
  int64_t TypeFilter = 0;
  /// Table offset of the next action, or nullopt for the end of the chain.
  std::optional<uint64_t> NextActionOffset;

  bool isCleanup() const { return TypeFilter == 0; }
  bool isCatch() const { return TypeFilter > 0; }
  bool isExceptionSpecification() const { return TypeFilter < 0; }
};

/// One protected region.  A zero landing pad means the region has no local
/// handler: an exception propagates out of the frame directly, which for the
/// `call-site` model is how a `noexcept` boundary or a plain call is spelled.
///
/// The SJLJ form fills a strict subset of this: an entry there is selected by
/// counting rather than by address, so \ref CallSiteIndex is what names it and
/// \ref GuardedRange and \ref LandingPadVA stay empty.  Everything the entry
/// reaches past that point — its action chain, and through it the catch types
/// and exception specifications — is the same table read the same way.
struct ItaniumCallSite {
  ExceptionAddressRange GuardedRange;
  va_t LandingPadVA = 0;
  /// Byte offset into the action table of the first action, or nullopt when
  /// the call site declared no action (an unconditional cleanup landing pad).
  std::optional<uint64_t> FirstActionOffset;
  /// SJLJ form only: the 1-based number that selects this entry, and zero in
  /// the address form.
  ///
  /// The frame stores this number into its own function context ahead of each
  /// call that can throw, and the personality reaches the entry by counting
  /// from the start of the table.  Nothing in the record says which code the
  /// entry covers, because nothing in the record has to: the stores are what
  /// say it, and they are in the function rather than in the table.
  uint64_t CallSiteIndex = 0;
  /// Native encoded fields, retained for provenance and regeneration.
  ///
  /// In the SJLJ form only the last two are written, and \ref NativeLandingPad
  /// is not an offset from anything: it selects a pad through the dispatch
  /// switch that the function's `setjmp` receiver runs.  The ABI's own
  /// "landing pad" number for the entry is one more than it, a bias that
  /// exists so the unwinder's generic "zero means no handler" test cannot fire
  /// on a form that has no way to spell that.
  uint64_t NativeStart = 0;
  uint64_t NativeLength = 0;
  uint64_t NativeLandingPad = 0;
  uint64_t NativeActionRecord = 0;
};

/// One type-table slot.  `TypeInfoVA` is zero for the catch-all slot, which
/// the ABI spells as a null `std::type_info*`.
struct ItaniumTypeEntry {
  /// 1-based index as named by a positive action filter.
  uint64_t Index = 0;
  va_t TypeInfoVA = 0;
  /// For an indirect encoding, the address of the cell the `std::type_info*`
  /// was loaded through.  A cell bound at load time holds a placeholder in the
  /// file image, so this is what lets the RTTI be named from the binding.
  va_t TypeInfoSlotVA = 0;
  /// Mangled RTTI symbol (`_ZTI...`) or the `std::type_info::__type_name`
  /// string, whichever could be proven; empty when neither could be.
  std::string TypeName;
  bool IsCatchAll = false;
};

/// One exception-specification list, named by a negative action filter.
struct ItaniumExceptionSpec {
  /// 1-based index as named by `-Index`.
  uint64_t Index = 0;
  /// Type-table indices the specification permits; an empty list is
  /// `throw()`/`noexcept`.
  std::vector<uint64_t> TypeIndices;
};

/// A fully decoded `.gcc_except_table` record.
struct ItaniumEHInfo {
  va_t LSDAVA = 0;
  /// Encoding and resolved base for landing-pad addresses.
  uint8_t LandingPadBaseEncoding = 0xFF;
  va_t LandingPadBase = 0;
  /// Encoding and resolved base of the type table.  `TypeTableVA` addresses
  /// the slot *past* the last entry because the table grows downward.
  uint8_t TypeTableEncoding = 0xFF;
  va_t TypeTableVA = 0;
  uint8_t CallSiteEncoding = 0xFF;
  uint64_t CallSiteTableLength = 0;
  std::vector<ItaniumCallSite> CallSites;
  std::vector<ItaniumAction> Actions;
  std::vector<ItaniumTypeEntry> TypeTable;
  std::vector<ItaniumExceptionSpec> ExceptionSpecs;
  /// True when the record uses the `.gcc_except_table` call-site table; false
  /// when it uses the SJLJ call-site form, whose "ranges" are call-site
  /// indices rather than addresses.
  bool IsCallSiteAddressForm = true;

  /// True when no call site names a catch or exception-specification action,
  /// so every landing pad is destructor cleanup.  A Rust frame compiled with
  /// `panic=unwind` and no `catch_unwind` has exactly this shape.
  bool isCleanupOnly() const {
    for (const ItaniumAction &A : Actions)
      if (!A.isCleanup())
        return false;
    return true;
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHITANIUM_H
