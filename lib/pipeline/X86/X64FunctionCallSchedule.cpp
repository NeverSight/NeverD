//===- X64FunctionCallSchedule.cpp - Scheduled call interfaces -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep scheduled call views on the existing instruction/event owner.
//===----------------------------------------------------------------------===//

#include "X64FunctionCallSchedule.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

llvm::StringRef x64ScheduledCallParameters(bool PagedMemory, bool CallSite) {
  if (PagedMemory)
    return CallSite
               ? "(neverd_x64_state_v6 *state, uint8_t *memory_context, "
                 "uint64_t *budget,\n"
                 "    neverd_x64_call_site_context_v1 *site, "
                 "neverd_x64_interruptibility_v1 *shadows, uint64_t "
                 "*poll_budget,\n"
                 "    neverd_x64_peek_interrupt_v1 peek, "
                 "neverd_x64_accept_interrupt_v1 accepted, void *event_context"
               : "(neverd_x64_state_v6 *state, uint8_t *memory_context, "
                 "uint64_t *budget,\n"
                 "    neverd_x64_call_context_v1 *call, "
                 "neverd_x64_interruptibility_v1 *shadows, uint64_t "
                 "*poll_budget,\n"
                 "    neverd_x64_peek_interrupt_v1 peek, "
                 "neverd_x64_accept_interrupt_v1 accepted, void *event_context";
  return CallSite
             ? "(neverd_x64_state_v6 *state, uint64_t *budget,\n"
               "    neverd_x64_call_site_context_v1 *site, "
               "neverd_x64_interruptibility_v1 *shadows, uint64_t "
               "*poll_budget,\n"
               "    neverd_x64_peek_interrupt_v1 peek, "
               "neverd_x64_accept_interrupt_v1 accepted, void *event_context"
             : "(neverd_x64_state_v6 *state, uint64_t *budget,\n"
               "    neverd_x64_call_context_v1 *call, "
               "neverd_x64_interruptibility_v1 *shadows, uint64_t "
               "*poll_budget,\n"
               "    neverd_x64_peek_interrupt_v1 peek, "
               "neverd_x64_accept_interrupt_v1 accepted, void *event_context";
}

void emitX64ScheduledCallArguments(llvm::raw_ostream &OS, bool PagedMemory,
                                   llvm::StringRef CallContext) {
  OS << "(state, " << (PagedMemory ? "memory_context, " : "") << "budget, ";
  if (!CallContext.empty())
    OS << CallContext << ", ";
  OS << "shadows, poll_budget, peek, accepted, event_context)";
}

std::string x64ScheduledCallSiteStepName(va_t Address) {
  return "neverd_x64_scheduled_call_site_" + llvm::utohexstr(Address) + "_v6";
}

} // namespace neverd
