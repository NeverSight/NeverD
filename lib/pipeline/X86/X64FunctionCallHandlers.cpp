//===- X64FunctionCallHandlers.cpp - Pending CALL handler returns --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep handler effects in the original state and reuse the bounded drivers.
//===----------------------------------------------------------------------===//

#include "X64FunctionCallHandlers.h"

#include "X64FunctionCallSchedule.h"
#include "X64FunctionContext.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>

namespace neverd {

void emitX64FunctionCallHandlerContext(llvm::raw_ostream &OS, bool Debug) {
  const auto Names = x64FunctionContextNames(Debug);
  OS << R"C(
/* Optional selected-handler continuation for a pending original CALL.
 * Begin only after the producer supplies a delivered event/fault handler
 * context. This declaration is not inferred from PC/SP. Retain this object
 * and its site across pauses; all controller storage is disjoint from native
 * state, budgets, producer/memory contexts and guest RAM.
 * Ready records a completed privilege return to the original CALL, including
 * when that return consumed the last instruction or poll-budget unit. */
typedef struct neverd_x64_call_site_handler_context_v1 {
    uint64_t Site, ReturnSlot, Ready;
} neverd_x64_call_site_handler_context_v1;
)C";
  OS << "int " << Names.HandlerBegin << "(\n    const " << Names.SiteContext
     << " *site,\n    neverd_x64_call_site_handler_context_v1 *handler) {\n";
  OS << R"C(
    if (site->Prepared != 1 ||
        site->Callee.Function.Phase != NEVERD_X64_FUNCTION_UNINITIALIZED)
        return 0;
    handler->Site = site->Site;
    handler->ReturnSlot = site->Callee.Function.ReturnSlot;
    handler->Ready = 0;
    return 1;
}
)C";
}

void emitX64FunctionCallHandlerResume(llvm::raw_ostream &OS, va_t Site,
                                      llvm::StringRef Name,
                                      llvm::StringRef Resume,
                                      llvm::StringRef Parameters,
                                      bool PagedMemory, bool ScheduledEvents) {
  using Exit = analysis::X64ContinuationExitV1;
  const auto ResumeSite = [&] {
    OS << Resume;
    if (ScheduledEvents)
      emitX64ScheduledCallArguments(OS, PagedMemory, "site");
    else if (PagedMemory)
      OS << "(state, context, budget, site)";
    else
      OS << "(state, budget, environment_budget, site, environment, "
            "environment_context)";
  };
  OS << "\nneverd_x64_call_result_v1 " << Name << Parameters
     << ", neverd_x64_call_site_handler_context_v1 *handler) {\n"
        "    neverd_x64_call_result_v1 result = {"
     << uint64_t(Exit::InvalidState)
     << ", 0, 0};\n"
        "    if (site->Prepared != 1 || site->Site != 0x"
     << llvm::utohexstr(Site)
     << "ULL) return result;\n"
        "    if (site->Callee.Function.Phase != "
        "NEVERD_X64_FUNCTION_UNINITIALIZED) return ";
  ResumeSite();
  OS << ";\n"
        "    if (handler->Site != site->Site ||\n"
        "        handler->ReturnSlot != site->Callee.Function.ReturnSlot ||\n"
        "        handler->Ready > 1) return result;\n"
        "    if (!handler->Ready) {\n"
        "        result.Status = ";
  // A region interface has no function activation to complete in the handler.
  // In particular, do not use run_v6: its environment could consume a
  // privilege-return boundary before this owner checks the actual PC/SP.
  if (ScheduledEvents)
    OS << "neverd_x64_continue_" << (PagedMemory ? "paged_" : "")
       << "events_v6(state, " << (PagedMemory ? "memory_context, " : "")
       << "budget, shadows, poll_budget, peek, accepted, event_context)";
  else if (PagedMemory)
    OS << "neverd_x64_run_paged_v6(state, context, budget)";
  else
    OS << "neverd_x64_continue_v6(state, budget)";
  OS << ";\n"
        "        if (result.Status != "
     << uint64_t(Exit::PrivilegeReturn) << " || state->PC != 0x"
     << llvm::utohexstr(Site)
     << "ULL ||\n"
        "            state->GPR[4] != handler->ReturnSlot) return result;\n";
  if (PagedMemory)
    OS << "        if (!(neverd_x64_paged_progress_v1("
       << (ScheduledEvents ? "memory_context" : "context") << ") & "
       << analysis::kX64PagedInstructionCompleted << ")) return result;\n";
  OS << "        handler->Ready = 1;\n"
        "    }\n"
        "    result = ";
  ResumeSite();
  OS << ";\n"
        "    if (site->Callee.Function.Phase == "
        "NEVERD_X64_FUNCTION_UNINITIALIZED &&\n"
        "        result.Status == "
     << uint64_t(Exit::Exception)
     << ") handler->Ready = 0;\n"
        "    return result;\n}\n";
}
} // namespace neverd
