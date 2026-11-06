//===- X64ContinuationSchedule.cpp - Instruction-boundary events --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose original instruction steps with shared IA-32e event delivery.
//===----------------------------------------------------------------------===//

#include "X64ContinuationSchedule.h"

#include "X64FunctionCallSchedule.h"
#include "X64FunctionCallSites.h"
#include "X64FunctionContext.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>

namespace neverd {

namespace {

void emitCompletedShadows(llvm::raw_ostream &OS, llvm::StringRef Indent,
                          bool Paged) {
  OS << Indent
     << "shadows->STIBlocked = (status == 0 && sti && !(old_flags & 512)"
     << (Paged ? " && (state->RFlags & 512)" : "") << ");\n";
  OS << Indent
     << "shadows->SSBlocked = (status == 0 && mov_ss && "
        "!shadows->SSBlocked);\n";
}

void emitScheduledDriver(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    bool FollowInternalCalls, bool FunctionContext,
    const analysis::X64ContinuationSourceOptions &Options, llvm::StringRef Name,
    const X64FunctionCallSitePauses *CallSitePauses) {
  const bool Paged = Options.PagedMemory;
  if (FunctionContext)
    OS << "/* Retain the activation across event and instruction boundaries.\n"
          " * Its storage is disjoint from state, shadows, budgets, both\n"
          " * contexts and guest RAM; the producer owns return-cell lifetime. "
          "*/\n";
  OS << "uint64_t " << Name << "(\n    neverd_x64_state_v6 *state, "
     << (Paged ? "uint8_t *memory_context, " : "") << R"C(uint64_t *budget,
    neverd_x64_interruptibility_v1 *shadows, uint64_t *poll_budget,
    neverd_x64_peek_interrupt_v1 peek,
    neverd_x64_accept_interrupt_v1 accepted, void *context)C";
  OS << (FunctionContext ? ", neverd_x64_function_context_v1 *function" : "")
     << ") {\n";
  if (FunctionContext)
    emitX64FunctionContextGuard(OS);
  OS << R"C(    uint64_t last = 8;
    if (!*budget) return 9;
    if (!peek || !accepted || shadows->STIBlocked > 1 || shadows->SSBlocked > 1 ||
        shadows->NMIBlockedBySTI > 1 ||
        (shadows->STIBlocked && !(state->RFlags & 512))) return 5;
    for (unsigned i = 0; i < sizeof(shadows->Reserved); ++i)
        if (shadows->Reserved[i]) return 5;
    while (*budget) {
        uint64_t status;
        int sti, mov_ss;
        switch (state->PC) {
)C";
  for (const auto &Unit : Units) {
    OS << "        case 0x" << llvm::utohexstr(Unit.Address) << "ULL: ";
    if (CallSitePauses && CallSitePauses->emit(OS, Unit.Address, ""))
      continue;
    OS << "status = ";
    if (Paged)
      OS << (Options.PagedDebug ? "neverd_x64_paged_debug_validate_v1"
                                : "neverd_x64_paged_validate_v1")
         << "(memory_context, (uint8_t *)state, 0x"
         << llvm::utohexstr(Unit.Address) << "ULL, "
         << Unit.EntryValidation->Name << "_paged_adapter, "
         << *Options.PagingAddressBits << ")";
    else
      OS << Unit.EntryValidation->Name << "((uint8_t *)state)";
    OS << "; sti = " << unsigned(Unit.IsSti)
       << "; mov_ss = " << unsigned(Unit.IsMovSS) << "; break;\n";
  }
  OS << R"C(        default: return last == 2 ? 2 : 8;
        }
        if (status) return status;
)C";
  OS << "        if (!*poll_budget) return "
     << uint64_t(analysis::X64ContinuationExitV1::EventBudgetExhausted)
     << ";\n";
  OS << R"C(
        neverd_x64_pending_interrupt_v1 pending = {0};
        --*poll_budget;
        int ready = peek(state, shadows, &pending, context);
        if (ready != 0 && ready != 1) return 5;
        if (ready) {
            if (pending.Kind != 2 && (pending.Kind != 3 || pending.Vector != 2))
                return 5;
            neverd_x64_event_v1 event = {0};
            event.SavedPC = state->PC;
            event.Vector = pending.Vector;
            event.Kind = pending.Kind;
            event.STIBlocked = shadows->STIBlocked;
            event.SSBlocked = shadows->SSBlocked;
            event.NMIBlockedBySTI = shadows->NMIBlockedBySTI;
)C";
  if (Paged)
    OS << "            status = neverd_x64_paged_event_v1(memory_context, "
          "(uint8_t *)state, (const uint8_t *)&event, "
       << *Options.MXCSRMask << ");\n"
       << "            if (neverd_x64_paged_progress_v1(memory_context) & "
       << analysis::kX64PagedEventAccepted << ") {\n";
  else
    OS << "            status = neverd_x64_deliver_event_v6(state, &event);\n"
          "            if (status == 7) {\n";
  OS << R"C(                shadows->STIBlocked = shadows->SSBlocked = 0;
                accepted(&pending, context);
                return status;
            }
            if (status != 15) return status;
        }
        const uint64_t old_flags = state->RFlags;
        switch (state->PC) {
)C";
  for (const auto &Unit : Units) {
    OS << "        case 0x" << llvm::utohexstr(Unit.Address) << "ULL:";
    if (FunctionContext && Unit.IsNearReturn) {
      OS << "\n";
      emitX64FunctionContextStep(OS, Unit, "            ", Paged,
                                 "memory_context", [&](llvm::StringRef Indent) {
                                   emitCompletedShadows(OS, Indent, Paged);
                                 });
      OS << "            break;\n";
    } else
      OS << " status = " << Unit.Function.Name
         << (Options.PagedDebug ? "_paged_debug_step(state, memory_context, "
                                  "shadows->SSBlocked); break;\n"
             : Paged            ? "_paged_step(state, memory_context); break;\n"
                                : "((uint8_t *)state); break;\n");
  }
  OS << R"C(        default: return 5;
        }
)C";
  if (Paged)
    OS << "        if (!(neverd_x64_paged_progress_v1(memory_context) & "
       << analysis::kX64PagedInstructionAttempted << ")) return status;\n";
  else
    OS << "        if (status == 4 || status == 5) return status;\n";
  OS << R"C(        --*budget;
)C";
  emitCompletedShadows(OS, "        ", Paged);
  if (Paged)
    OS << "        if (status == "
       << uint64_t(analysis::X64PagedExit::RepeatContinue) << ") status = 0;\n";
  if (FollowInternalCalls)
    OS << R"C(        if (status == 1 || status == 3) {
            if (!neverd_x64_scheduled_pc_v6(state->PC)) return status;
            last = status;
            continue;
        }
)C";
  OS << R"C(        if (status != 0 && status != 2) return status;
        last = status;
    }
    return 9;
}
)C";
}

} // namespace

void emitX64ContinuationSchedule(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    bool FollowInternalCalls, bool FunctionContext,
    const analysis::X64ContinuationSourceOptions &Options,
    llvm::ArrayRef<X64FunctionCallSite> CallSites,
    const X64FunctionCallSitePauses *CallSitePauses) {
  const bool Paged = Options.PagedMemory;
  OS << R"C(
/* Instruction-boundary IRQ/NMI producer. peek returns 0=no event, 1=pending;
 * every peek consumes one poll-budget unit. It must not acknowledge the event
 * or mutate state, shadows, budgets, selected code or guest RAM. The producer
 * owns priority/APIC/PIC eligibility (including CR8), coalescing and arrival.
)C";
  OS << (Paged ? " * accepted consumes the event once after admission, "
                 "including failed IDT entry.\n"
               : " * accepted is called exactly once after successful "
                 "delivery; it cannot fail.\n");
  OS << R"C( * Its only effect is acknowledging that event in the producer's context.
 * State, shadows, budgets and context are mutually disjoint and accessible.
 * No callback runs when entry validation, selection or a budget stops execution.
 * 16=poll budget exhausted. Other statuses retain the continuation contract.
 * Successful event entry consumes no instruction budget and stops at the handler.
 * Shadows survive all rejected/unexecuted steps and expire on completion or
 * delivered exceptions. Paged MOV SS can start a new inhibition window;
 * an already active MOV-SS window is consumed, not renewed. */
typedef struct neverd_x64_interruptibility_v1 {
    uint8_t STIBlocked, SSBlocked, NMIBlockedBySTI, Reserved[5];
} neverd_x64_interruptibility_v1;
typedef struct neverd_x64_pending_interrupt_v1 {
    uint8_t Vector, Kind; /* Kind=2 (IRQ) or 3 (NMI, Vector=2). */
} neverd_x64_pending_interrupt_v1;
typedef int (*neverd_x64_peek_interrupt_v1)(
    const neverd_x64_state_v6 *, const neverd_x64_interruptibility_v1 *,
    neverd_x64_pending_interrupt_v1 *, void *);
typedef void (*neverd_x64_accept_interrupt_v1)(
    const neverd_x64_pending_interrupt_v1 *, void *);

static int neverd_x64_scheduled_pc_v6(uint64_t pc) {
    switch (pc) {
)C";
  for (const auto &Unit : Units)
    OS << "    case 0x" << llvm::utohexstr(Unit.Address) << "ULL:\n";
  OS << R"C(        return 1;
    default: return 0;
    }
}

)C";
  const llvm::StringRef Name =
      Paged ? (FunctionContext ? "neverd_x64_continue_paged_events_function_v6"
                               : "neverd_x64_continue_paged_events_v6")
            : (FunctionContext ? "neverd_x64_continue_events_function_v6"
                               : "neverd_x64_continue_events_v6");
  emitScheduledDriver(OS, Units, FollowInternalCalls, FunctionContext, Options,
                      Name, CallSitePauses);
  if (FunctionContext)
    OS << "uint64_t neverd_x64_continue_" << (Paged ? "paged_" : "")
       << "events_v6(\n    neverd_x64_state_v6 *state, "
       << (Paged ? "uint8_t *memory_context, " : "") << R"C(uint64_t *budget,
    neverd_x64_interruptibility_v1 *shadows, uint64_t *poll_budget,
    neverd_x64_peek_interrupt_v1 peek,
    neverd_x64_accept_interrupt_v1 accepted, void *context) {
    return neverd_x64_continue_)C"
       << (Paged ? "paged_" : "") << "events_function_v6(state, "
       << (Paged ? "memory_context, " : "")
       << "budget, shadows, poll_budget, peek, accepted, context, 0);\n}\n";
  // The same scheduler stops after each authenticated original CALL. These
  // helpers have no pause marker or callee activation: the bridge establishes
  // the activation only after attempted CALL success and never replays it.
  for (const auto &Site : CallSites) {
    OS << "static ";
    emitScheduledDriver(
        OS, llvm::ArrayRef(Site.Instruction, 1), false, false, Options,
        x64ScheduledCallSiteStepName(Site.Instruction->Address), nullptr);
  }
}

} // namespace neverd
