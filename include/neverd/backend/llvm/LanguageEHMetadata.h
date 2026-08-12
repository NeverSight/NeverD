//===- LanguageEHMetadata.h - Lifted language EH markers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Markers attached to a lifted function whose exception behaviour was lowered
/// to native LLVM constructs rather than left in metadata.  The Windows models
/// have their own lossless schema in `WindowsEHMetadata.h`; the models here
/// need no schema of their own because what they recover is already spelled by
/// the IR itself — an `invoke` names the protected call and a `landingpad`
/// names the clauses — so all that remains to record is which model produced
/// it and how much of the table it accounted for.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LANGUAGEEHMETADATA_H
#define NEVERD_BACKEND_LLVM_LANGUAGEEHMETADATA_H

#include "llvm/ADT/StringRef.h"

namespace neverd::language_eh_md {

/// Attached to a function whose Itanium LSDA became `invoke`/`landingpad`.
inline constexpr llvm::StringLiteral ItaniumAttachment("neverd.itanium.eh");

enum ItaniumOperand : unsigned {
  /// Personality symbol the function was given.
  Personality = 0,
  /// Landing pads the LSDA named that became a `landingpad`.
  LoweredPads,
  /// Landing pads the LSDA named that could not be lowered, and so are
  /// reachable only through the decoded table.  Nonzero means this function's
  /// IR describes less than its table does.
  SkippedPads,
  /// Calls the call-site table protected that became an `invoke`.
  LoweredCalls,
  ItaniumOperandCount,
};

} // namespace neverd::language_eh_md

#endif // NEVERD_BACKEND_LLVM_LANGUAGEEHMETADATA_H
