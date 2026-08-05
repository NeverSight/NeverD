//===- MedTypePass.h - Type inference pass for MedIR --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the type inference pass that annotates MedFunc with inferred
/// return type, parameter types, and local variable types.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDTYPEPASS_H
#define NEVERD_IR_MED_MEDTYPEPASS_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

void inferMedTypes(MedFunc &Func, Arch TheArch);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDTYPEPASS_H
