//===- RuntimeBoundsSanitizer.h - Exact write bounds guards -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Transactionally installs the last LLVM mutation for strict native write
/// sanitization.  Every guard must resolve to exactly one persistent v1
/// callsite record.  The module is cloned, instrumented, and verified before
/// ownership is replaced, so every failure leaves the caller's module intact.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_SAFETY_RUNTIMEBOUNDSSANITIZER_H
#define NEVERD_PASS_IR_SAFETY_RUNTIMEBOUNDSSANITIZER_H

#include "neverd/Common.h"
#include "neverd/backend/llvm/SafetyCallsiteMetadata.h"
#include "neverd/safety/RuntimeSanitizer.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace neverd::safety {

/// Why the LLVM last-mutation stage refused a guard plan.
///
/// Values are a public typed result contract.  Append new outcomes; do not
/// renumber existing ones.
enum class RuntimeBoundsSanitizerError : uint8_t {
  None = 0,
  NullModule = 1,
  UnsupportedPlanVersion = 2,
  InvalidGuard = 3,
  DuplicateGuard = 4,
  MalformedMetadata = 5,
  MissingTarget = 6,
  DuplicateTarget = 7,
  MetadataMismatch = 8,
  OperandIndexOutOfRange = 9,
  OperandTypeMismatch = 10,
  VerificationFailed = 11,
  UnexpectedTarget = 12,
};
static_assert(
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::None) == 0 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::NullModule) == 1 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::UnsupportedPlanVersion) ==
        2 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::InvalidGuard) == 3 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::DuplicateGuard) == 4 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::MalformedMetadata) == 5 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::MissingTarget) == 6 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::DuplicateTarget) == 7 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::MetadataMismatch) == 8 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::OperandIndexOutOfRange) ==
        9 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::OperandTypeMismatch) ==
        10 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::VerificationFailed) ==
        11 &&
    static_cast<uint8_t>(RuntimeBoundsSanitizerError::UnexpectedTarget) == 12);

const char *toString(RuntimeBoundsSanitizerError Error);

struct RuntimeBoundsSanitizerResult {
  /// True only after a complete candidate module passed LLVM verification and
  /// was atomically installed into the caller's owning pointer.
  bool Complete = false;
  RuntimeBoundsSanitizerError Error = RuntimeBoundsSanitizerError::None;
  std::optional<safety_callsite_md::SafetyCallsiteOccurrence> FailureOccurrence;
  std::string Detail;
  /// Sorted, unique original native entries whose writes are now guarded.
  std::vector<va_t> GuardedOriginalEntries;
};

/// Install exact-capacity write guards as the final LLVM mutation.
///
/// The input module is never modified in place.  A clone is validated,
/// transformed, and verified; only a complete success replaces \p Module.
[[nodiscard]] RuntimeBoundsSanitizerResult
applyRuntimeBoundsSanitizer(std::unique_ptr<llvm::Module> &Module,
                            llvm::ArrayRef<RuntimeSanitizerGuard> Guards);

} // namespace neverd::safety

#endif // NEVERD_PASS_IR_SAFETY_RUNTIMEBOUNDSSANITIZER_H
