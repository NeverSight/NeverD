//===- MIPSMergeValues.h - Complete merge-load value proofs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source-only elimination of unobserved incoming bits in complete MIPS pairs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MIPS_MIPSMERGEVALUES_H
#define NEVERD_IR_MED_MIPS_MIPSMERGEVALUES_H

#include "neverd/ArchSupport.h"

namespace neverd {

struct LowFunc;
struct MedFunc;

/// Run after SSA, byte projection and DCE, before ABI inference. Only adjacent
/// ordinary instructions in a single-block leaf are currently proved. Both
/// memory operations remain, including their access order and fault behavior.
/// Returns the number of first-core old-value operands made independent.
unsigned discardUnobservedMIPSMergeInputs(MedFunc &Function,
                                          const LowFunc &Physical, Arch Target);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSMERGEVALUES_H
