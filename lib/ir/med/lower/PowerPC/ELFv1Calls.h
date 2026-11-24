//===- ELFv1Calls.h - PPC64 descriptor and TOC lowering --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover source-level operands for PPC64 ELFv1 machine operations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_POWERPC_ELFV1CALLS_H
#define NEVERD_LIB_IR_MED_LOWER_POWERPC_ELFV1CALLS_H

namespace neverd {
struct MedFunc;
struct BinaryImage;
/// Project a machine call through the descriptor's entry, TOC and static
/// chain to the ABI function pointer consumed by both source backends.
void recoverPPC64ELFv1(MedFunc &Function, const BinaryImage *Image);
} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_POWERPC_ELFV1CALLS_H
