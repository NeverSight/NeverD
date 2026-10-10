//===- X86RegistrationTry.h - Synchronous registration regions --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86REGISTRATIONTRY_H
#define NEVERD_IR_HIGH_X86REGISTRATIONTRY_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// Exact ordinary CFG ranges for a closed synchronous component. All calls
/// terminate and are protected by this try; runtime resumes are independent.
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
