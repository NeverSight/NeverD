//===- X64FunctionContext.h - Physical function activation -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit an explicit entry-return boundary without inventing source arguments.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCONTEXT_H
#define NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCONTEXT_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class raw_ostream;
}
namespace neverd {
namespace analysis {
struct X64ContinuationInstructionSource;
}

/// Begin only at an explicitly requested root. Record its physical return-cell
/// address, not the cell's value; guest writes still own the eventual return
/// PC.
void emitX64FunctionContext(llvm::raw_ostream &OS, unsigned Version,
                            llvm::ArrayRef<va_t> Entries);

/// A completed activation is idempotent; an uninitialized one is refused.
/// A null context selects the original driver through the adapter below.
void emitX64FunctionContextGuard(llvm::raw_ostream &OS);

/// Only a successful, authenticated near RET through the entry cell completes
/// this activation. It consumes one instruction unit before stopping. All
/// other steps use the ordinary driver's budget and boundary handling.
void emitX64FunctionContextStep(
    llvm::raw_ostream &OS,
    const analysis::X64ContinuationInstructionSource &Unit,
    llvm::StringRef Indent);

/// Keep the original context-free entry point without duplicating its driver.
void emitX64FunctionContextAdapter(llvm::raw_ostream &OS, unsigned Version);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCONTEXT_H
