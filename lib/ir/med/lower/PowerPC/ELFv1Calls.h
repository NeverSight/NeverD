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

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"

namespace neverd {
struct BinaryImage;
struct PPC64LinkageCalls {
  llvm::DenseMap<int, va_t> RequiredTOCs;
  /// Address of the caller's reload and the call occurrence preceding it.
  llvm::DenseMap<va_t, uint32_t> CallerRestores;
};
/// Materialize the linkage area's save before SSA. Each snapshot records
/// which TOC the reaching r2 must prove before the direct call is published.
PPC64LinkageCalls preparePPC64LinkageCalls(MedFunc &Function,
                                           const BinaryImage &Image, MedVar TOC,
                                           MedVar SP, int &NextVarId);
/// Project a machine call through the descriptor's entry, TOC and static
/// chain to the ABI function pointer consumed by both source backends.
void recoverPPC64ELFv1(MedFunc &Function, const BinaryImage *Image,
                       const PPC64LinkageCalls &Calls);
} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_POWERPC_ELFV1CALLS_H
