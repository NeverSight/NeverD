//===- X86RegistrationCallResult.h - PE32 result uses ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check one current SSA occurrence before discarding its physical result.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONCALLRESULT_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONCALLRESULT_H

#include "neverd/Common.h"

#include <cstddef>
#include <optional>

namespace neverd {
struct MedFunc;
struct RegistrationCallFrameEffect;

enum class X86RegistrationCallResultUse { Unused, Observed };

/// Classify a complete current result-use graph. Malformed SSA and exhausted
/// work return no classification. An observation grants no native return ABI.
std::optional<X86RegistrationCallResultUse> getX86RegistrationCallResultUse(
    const MedFunc &Function, const RegistrationCallFrameEffect &Call,
    size_t &Work, bool Indirect = false, va_t RuntimeFinally = 0);

/// Follow only SSA copies, integer extensions and PHIs. Every other use,
/// including a call's implicit arguments, observes the result. Duplicate or
/// missing definitions, ambiguous occurrences and exhausted budgets reject.
/// This proves absence of a source observation, never a language return type.
/// Indirect selects an exact constant IAT-call occurrence; its runtime identity
/// and argument ABI still require the caller's separate source proof.
/// RuntimeFinally permits an incidental result at a checked near return only
/// when that entire finally body has no ordinary caller or other entry kind.
bool hasUnusedX86RegistrationCallResult(const MedFunc &Function,
                                        const RegistrationCallFrameEffect &Call,
                                        size_t &Work, bool Indirect = false,
                                        va_t RuntimeFinally = 0);
} // namespace neverd

#endif
