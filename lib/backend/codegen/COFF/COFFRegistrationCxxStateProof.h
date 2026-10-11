//===- COFFRegistrationCxxStateProof.h --------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_COFFREGISTRATIONCXXSTATEPROOF_H
#define NEVERD_COFFREGISTRATIONCXXSTATEPROOF_H

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"

namespace neverd::coff_registration {
struct CxxGeneratedTry {
  va_t RowVA = 0;
  int32_t Low = -1;
  int32_t High = -1;
  int32_t CatchHigh = -1;
};

llvm::Error validateCxxGeneratedStates(
    const CxxExceptionInfo &Source, const CompiledImage &Compiled,
    const std::vector<CxxGeneratedTry> &Tries, va_t Unwind, uint32_t MaxState,
    const std::map<uint32_t, const CompiledWinEHSemanticRecord *> &Cleanups,
    COFFRegistrationCxxTableReceipt &Receipt);
} // namespace neverd::coff_registration
#endif
