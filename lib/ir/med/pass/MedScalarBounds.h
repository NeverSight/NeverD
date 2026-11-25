//===- MedScalarBounds.h - Unsigned bounds of SSA values -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounds on ordinary integer values at their original machine width.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_PASS_MEDSCALARBOUNDS_H
#define NEVERD_LIB_IR_MED_PASS_MEDSCALARBOUNDS_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;
struct MedFunc;
struct MedVar;

/// Use one definition index for the queries. A join without an independent
/// machine-value bound, or a value with relocatable bits, has no fixed maximum.
std::vector<std::optional<uint64_t>>
unsignedScalarUpperBounds(const MedFunc &Func, const BinaryImage &Image,
                          llvm::ArrayRef<MedVar> Values);
} // namespace neverd

#endif // NEVERD_LIB_IR_MED_PASS_MEDSCALARBOUNDS_H
