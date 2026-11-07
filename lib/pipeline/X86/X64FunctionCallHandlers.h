//===- X64FunctionCallHandlers.h - Pending CALL handler returns -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose selected handler instructions with an existing pending CALL.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLHANDLERS_H
#define NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLHANDLERS_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

namespace llvm {
class raw_ostream;
}

namespace neverd {
void emitX64FunctionCallHandlerContext(llvm::raw_ostream &OS,
                                       bool Debug = false);

/// Append a separate resume interface; ordinary site resumes and all
/// instruction drivers retain their existing behavior. The producer declares
/// a delivered handler context; only an actual successful privilege return to
/// the original CALL coordinates permits a retry. Paged returns additionally
/// require instruction-attempt progress from the existing execution owner.
void emitX64FunctionCallHandlerResume(llvm::raw_ostream &OS, va_t Site,
                                      llvm::StringRef Name,
                                      llvm::StringRef Resume,
                                      llvm::StringRef Parameters,
                                      bool PagedMemory, bool ScheduledEvents);
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLHANDLERS_H
