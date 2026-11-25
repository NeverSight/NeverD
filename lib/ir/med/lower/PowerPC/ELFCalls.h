//===- ELFCalls.h - PPC64 descriptor and TOC lowering --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover source-level operands for the PPC64 ELF entry and linkage
/// conventions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_POWERPC_ELFCALLS_H
#define NEVERD_LIB_IR_MED_LOWER_POWERPC_ELFCALLS_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace neverd {
struct BinaryImage;
struct PPC64LinkageCalls {
  llvm::DenseMap<int, va_t> RequiredTOCs;
  llvm::DenseMap<int, va_t> RequiredGlobalEntries;
  llvm::DenseSet<int> IndirectEntrySnapshots;
  llvm::DenseMap<uint32_t, va_t> CallTargets;
  /// Address of the caller's reload and the call occurrence preceding it.
  llvm::DenseMap<va_t, uint32_t> CallerRestores;
};
/// Materialize the linkage area's save before SSA. Each snapshot records
/// which TOC the reaching r2 must prove before the direct call is published.
PPC64LinkageCalls preparePPC64LinkageCalls(MedFunc &Function,
                                           const BinaryImage &Image, MedVar TOC,
                                           MedVar SP, MedVar R12,
                                           int &NextVarId);
/// Bind the established entry registers and restore the caller's TOC before
/// projecting machine calls to native function pointers or direct calls.
void recoverPPC64ELFCalls(MedFunc &Function, const BinaryImage *Image,
                          const PPC64LinkageCalls &Calls, bool Descriptors);
} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_POWERPC_ELFCALLS_H
