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
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class raw_ostream;
}
namespace neverd {
namespace analysis {
struct X64ContinuationInstructionSource;
}

/// Debug activations retain a completion stop/value. Keep their larger storage
/// and entry points distinct from the existing two-word v1 interface.
struct X64FunctionContextNames {
  llvm::StringRef Context, Begin, BeginAt, PagedDriver, ScheduledDriver;
  llvm::StringRef CallContext, SiteContext, HandlerBegin;
};
X64FunctionContextNames x64FunctionContextNames(bool Debug = false);
llvm::StringRef x64FunctionCallPrefix(bool Paged, bool Scheduled,
                                      bool Debug = false);

/// Begin only at an explicitly requested root. Record its entry return-cell
/// address (guest virtual in paged mode), not the cell's value; guest writes
/// still own the eventual return PC.
void emitX64FunctionContext(llvm::raw_ostream &OS, unsigned Version,
                            llvm::ArrayRef<va_t> Entries, bool Debug = false);

/// A completed activation is idempotent; an uninitialized one is refused.
/// A null context selects the original driver through the adapter below.
void emitX64FunctionContextGuard(llvm::raw_ostream &OS, bool Debug = false);

/// Only a successful, authenticated near RET through the entry cell completes
/// this activation. It consumes one instruction unit before stopping. All
/// other steps use the ordinary driver's budget and boundary handling.
/// Paged steps additionally require the executor's attempted-instruction bit;
/// its fault/backing/code-change outcomes never complete the activation.
/// A nonempty DebugSSShadow selects the v2 paged debug step and actual
/// completed transfer receipt; final event status alone does not prove RET
/// completion. OnReturn emits the enclosing driver's completion effects after
/// charging the instruction and before publishing the returned phase.
void emitX64FunctionContextStep(
    llvm::raw_ostream &OS,
    const analysis::X64ContinuationInstructionSource &Unit,
    llvm::StringRef Indent, bool PagedMemory = false,
    llvm::StringRef PagedContext = "context",
    llvm::function_ref<void(llvm::StringRef)> OnReturn = {},
    llvm::StringRef DebugSSShadow = {});

/// Keep the original context-free entry point without duplicating its driver.
void emitX64FunctionContextAdapter(llvm::raw_ostream &OS, unsigned Version);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCONTEXT_H
