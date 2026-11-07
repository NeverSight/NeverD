//===- X64FunctionCallSchedule.cpp - Scheduled call interfaces -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep scheduled call views on the existing instruction/event owner.
//===----------------------------------------------------------------------===//

#include "X64FunctionCallSchedule.h"

#include "X64FunctionContext.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

std::string x64ScheduledCallParameters(bool PagedMemory, bool CallSite,
                                       bool Debug) {
  const auto Names = x64FunctionContextNames(Debug);
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  OS << "(neverd_x64_state_v6 *state, "
     << (PagedMemory ? "uint8_t *memory_context, " : "")
     << "uint64_t *budget,\n    "
     << (CallSite ? Names.SiteContext : Names.CallContext)
     << (CallSite ? " *site, " : " *call, ")
     << "neverd_x64_interruptibility_v1 *shadows, uint64_t *poll_budget,\n"
        "    neverd_x64_peek_interrupt_v1 peek, "
        "neverd_x64_accept_interrupt_v1 accepted, void *event_context";
  return Result;
}

void emitX64ScheduledCallArguments(llvm::raw_ostream &OS, bool PagedMemory,
                                   llvm::StringRef CallContext) {
  OS << "(state, " << (PagedMemory ? "memory_context, " : "") << "budget, ";
  if (!CallContext.empty())
    OS << CallContext << ", ";
  OS << "shadows, poll_budget, peek, accepted, event_context)";
}

std::string x64ScheduledCallSiteStepName(va_t Address) {
  return (llvm::Twine("neverd_x64_scheduled_call_site_") +
          llvm::utohexstr(Address) + "_v6")
      .str();
}

} // namespace neverd
