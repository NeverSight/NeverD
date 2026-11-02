//===- MedIntegerReturnRange.h - Bounded integer call results --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Machine integer return bounds from complete ordinary leaf functions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDINTEGERRETURNRANGE_H
#define NEVERD_IR_MED_MEDINTEGERRETURNRANGE_H

#include "neverd/Common.h"

#include "llvm/ADT/DenseMap.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct BinaryImage;
struct LowFunc;
struct MedFunc;

/// Inclusive unsigned bounds on the complete primary integer return register.
/// These describe machine bits, not an inferred source return type.
struct MedIntegerReturnRange {
  Arch Architecture;
  uint16_t Size;
  uint64_t Min;
  uint64_t Max;
};

/// Phase-local facts keyed by entry in one image. Rebuild after replacing a
/// callee or its source contract; never retain these across image mutations.
using MedCallReturnRanges = llvm::DenseMap<va_t, MedIntegerReturnRange>;

/// Summarize all ordinary returns of a complete call-free body. Func must be
/// the corresponding MedIR conversion of Source. Unbounded results, incomplete
/// coverage, nonordinary control flow and exhausted work publish no fact.
std::optional<MedIntegerReturnRange>
integerReturnRange(const MedFunc &Func, const LowFunc &Source,
                   const BinaryImage &Image);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDINTEGERRETURNRANGE_H
