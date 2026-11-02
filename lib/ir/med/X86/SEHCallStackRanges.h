//===- SEHCallStackRanges.h - Source handler stack bounds ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bound physical CALL writes without choosing an exception-entry SP value.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_SEHCALLSTACKRANGES_H
#define NEVERD_IR_MED_X86_SEHCALLSTACKRANGES_H

#include "neverd/ir/med/MedStackAlignment.h"

#include <cstddef>
#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

namespace x86_seh {
std::map<const MedOp *, EntryStackOffsetRange> sourceHandlerCallStackRanges(
    const MedFunc &Func, const LowFunc &Source, const BinaryImage &Image,
    const std::map<const MedOp *, MedVar> &Stacks,
    const std::map<int, MedVar> &BlockEntries, size_t &Remaining);
} // namespace x86_seh
} // namespace neverd

#endif // NEVERD_IR_MED_X86_SEHCALLSTACKRANGES_H
