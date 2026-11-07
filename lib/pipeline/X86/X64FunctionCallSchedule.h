//===- X64FunctionCallSchedule.h - Scheduled call interfaces ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share event-producer parameters across checked entry and CALL-site views.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLSCHEDULE_H
#define NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLSCHEDULE_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace llvm {
class raw_ostream;
}

namespace neverd {

std::string x64ScheduledCallParameters(bool PagedMemory, bool CallSite,
                                       bool Debug = false);

/// Emit a call argument list. An empty context selects the scheduler itself;
/// call/resume wrappers additionally receive their retained activation context.
void emitX64ScheduledCallArguments(llvm::raw_ostream &OS, bool PagedMemory,
                                   llvm::StringRef CallContext);

/// A bounded scheduler that stops after this authenticated original CALL.
std::string x64ScheduledCallSiteStepName(va_t Address);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLSCHEDULE_H
