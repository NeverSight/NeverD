//===- X86RegistrationTry.h - Synchronous registration regions --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86REGISTRATIONTRY_H
#define NEVERD_IR_HIGH_X86REGISTRATIONTRY_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include <optional>
#include <set>

namespace neverd {

/// Ordinary code reachable from authenticated runtime resume entries. Shared
/// return tails stay outside lexical try bodies entered before dispatch.
std::optional<std::set<int>> registrationResumeClosure(const MedFunc &Med,
                                                       size_t &Work);

/// Exact ordinary CFG ranges for a closed synchronous component. All calls
/// are checked leaves or protected throws. Paths terminate or leave for an
/// independently entered runtime continuation.
std::vector<ExceptionAddressRange>
terminalRegistrationTryRanges(const MedFunc &Med, int32_t TryLow,
                              int32_t TryHigh);

/// Extract a closed, terminal ordinary component whose calls all belong to
/// one synchronous try. Runtime continuations can interrupt its address order.
/// Failure leaves both vectors unchanged.
bool extractTerminalRegistrationTry(HighFunc &Func, const MedFunc &Med,
                                    int32_t TryLow, int32_t TryHigh,
                                    std::vector<HighStmt> &Body,
                                    size_t &InsertAt);

/// Check the independent callback bodies of a previously structured inner
/// try before moving that node into an enclosing synchronous region.
bool checkNestedRegistrationTry(const HighStmt &Stmt, const MedFunc &Med,
                                int32_t TryLow, int32_t TryHigh, size_t &Work);

} // namespace neverd

#endif
