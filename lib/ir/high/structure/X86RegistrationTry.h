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

/// Extract a closed, terminal ordinary component whose calls all belong to
/// one synchronous try. Runtime continuations can interrupt its address order.
/// Failure leaves both vectors unchanged.
bool extractTerminalRegistrationTry(HighFunc &Func, const MedFunc &Med,
                                    std::vector<HighStmt> &Body,
                                    size_t &InsertAt);

} // namespace neverd

#endif
