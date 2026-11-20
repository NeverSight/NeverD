//===- LowToMedCallReturnDetail.h - Call result reads -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What a read of a register after a call takes from it, shared by the
/// passes that model a call's results past its return register.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_LOWTOMEDCALLRETURNDETAIL_H
#define NEVERD_LIB_IR_MED_LOWER_LOWTOMEDCALLRETURNDETAIL_H

#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <vector>

namespace neverd::lowtomed_detail {

/// Whether \p Ops[\p At] reads a register only to keep the bytes a write of
/// its low bytes leaves, as a partial register write lowers: `t = R & mask`,
/// whose mask clears at least the low byte, then `u = t | v` and `R = u`
/// within the next few operations.
bool keepsUpperBytesOfWrite(const std::vector<MedOp> &Ops, size_t At);

} // namespace neverd::lowtomed_detail

#endif // NEVERD_LIB_IR_MED_LOWER_LOWTOMEDCALLRETURNDETAIL_H
