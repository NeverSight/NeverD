//===- CSourceEvidence.h - Occurrence-bound source evidence -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Associate conditional analysis evidence with surviving source statements.

#ifndef NEVERD_BACKEND_C_CSOURCEEVIDENCE_H
#define NEVERD_BACKEND_C_CSOURCEEVIDENCE_H

#include "neverd/backend/c/CSourceMap.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {

class LLVMSourceMap;

/// The producer validates the occurrence against its current IR. The source
/// backend checks surviving emission anchors, never searches rendered C for
/// addresses or callee text. Evidence does not authenticate a native target,
/// ABI, immutable string, or permission to replace a call.
struct CSourceEvidence {
  va_t Function = 0;
  sigs::LibraryOccurrence Occurrence;
  std::string Text;
  /// Optional readable name for this call's result (a status for state ABIs).
  /// This is presentation only. Conflicting names are not installed.
  std::string ResultName;
  enum class ValueRole { CallResult, StoredValue };
  ValueRole Role = ValueRole::CallResult;
};

struct CSourceEvidenceResult {
  std::string Source;
  size_t Mapped = 0;
  size_t Unmapped = 0;
};

/// Name only an exact surviving call in its recorded function. No calls,
/// operands, types, attributes, control flow or memory effects are modified.
void nameLLVMSourceEvidence(const LLVMSourceMap &Sources,
                            llvm::ArrayRef<CSourceEvidence> Evidence);

/// Insert escaped comments at proven statement spans. Missing/ambiguous
/// anchors stay unmapped. Returns the annotated text and coverage counts;
/// the input map continues to describe the original unannotated text.
CSourceEvidenceResult
annotateCSourceEvidence(llvm::StringRef Source, const CSourceMap &Map,
                        llvm::ArrayRef<CSourceEvidence> Evidence);

} // namespace neverd
#endif
