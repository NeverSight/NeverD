//===- PICGlobalPointer.h - MIPS PIC entry GP projection --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_LOWER_MIPS_PICGLOBALPOINTER_H
#define NEVERD_IR_MED_LOWER_MIPS_PICGLOBALPOINTER_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>

namespace neverd {

struct BinaryImage;

/// Bind the completed entry-relative GP calculation in a linked PIC function.
std::optional<LowFunc> projectMIPSPICGlobalPointer(
    const LowFunc &Function, const BinaryImage &Image,
    llvm::function_ref<bool(va_t)> StaticSlot = nullptr);

} // namespace neverd

#endif // NEVERD_IR_MED_LOWER_MIPS_PICGLOBALPOINTER_H
