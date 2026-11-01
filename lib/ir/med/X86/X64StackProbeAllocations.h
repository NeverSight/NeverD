//===- X64StackProbeAllocations.h - Probed x64 PE allocations --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bound the dynamic allocation that follows an authenticated x64 PE page
/// probe, for physical CALL return-slot validation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_X64STACKPROBEALLOCATIONS_H
#define NEVERD_IR_MED_X86_X64STACKPROBEALLOCATIONS_H

#include "neverd/ir/med/MedProvenStackOffsets.h"

#include <cstddef>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct MedFunc;

namespace x64_stack_probe {
/// Query-local ranges at each exact `sub rsp, rax` that follows a
/// byte-authenticated probe CALL in the same block, with RAX unchanged and no
/// competing control flow, guard or exceptional entry in between. Images
/// other than x64 PE, and exhausted budgets, produce no ranges.
std::vector<ProvenStackOffsetRange>
probedStackAllocations(const MedFunc &Func, const LowFunc &Source,
                       const BinaryImage &Image, size_t &Remaining);
} // namespace x64_stack_probe
} // namespace neverd

#endif // NEVERD_IR_MED_X86_X64STACKPROBEALLOCATIONS_H
