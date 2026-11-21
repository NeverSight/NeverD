//===- MedMIPSRecordFrame.h - Bounded record-copy frame proof --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_PASS_MIPS_MEDMIPSRECORDFRAME_H
#define NEVERD_LIB_IR_MED_PASS_MIPS_MEDMIPSRECORDFRAME_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::mips {
struct RecordFrameCall {
  int64_t PrivateStart;
  unsigned NumericResultBytes;
};
using RecordFrameCallInfo =
    llvm::function_ref<std::optional<RecordFrameCall>(const MedOp &)>;

/// Prove the captured link on a single normal return through counted copies.
/// Unknown loop bounds, uninitialized private bytes and frame escapes reject.
bool preservesRecordFrameLink(const MedFunc &Function, const MedOp &Return,
                              Arch Target, RecordFrameCallInfo CallInfo,
                              size_t &Budget);
} // namespace neverd::mips
#endif
