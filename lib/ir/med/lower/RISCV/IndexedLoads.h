//===- IndexedLoads.h - RISC-V absolute table reads ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover image storage at a bounded RISC-V memory use.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_RISCV_INDEXEDLOADS_H
#define NEVERD_LIB_IR_MED_LOWER_RISCV_INDEXEDLOADS_H

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd {
class ELFIndexedData;
struct BinaryImage;
struct MedFunc;
struct MedOp;
struct MedVar;

namespace riscv {
void recoverIndexedLoads(MedFunc &Func, const BinaryImage &Image,
                         const ELFIndexedData *SharedData, int &NextVarId,
                         llvm::function_ref<const MedOp *(const MedVar &)> Def);
} // namespace riscv
} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_RISCV_INDEXEDLOADS_H
