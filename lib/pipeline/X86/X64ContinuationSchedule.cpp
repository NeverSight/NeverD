//===- X64ContinuationSchedule.cpp - Instruction-boundary events --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose original instruction steps with shared IA-32e event delivery.
//===----------------------------------------------------------------------===//

#include "X64ContinuationSchedule.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>

namespace neverd {

void emitX64ContinuationSchedule(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    bool FollowInternalCalls) {
  OS << R"C(
/* Instruction-boundary IRQ/NMI producer. peek returns 0=no event, 1=pending;
 * every peek consumes one poll-budget unit. It must not acknowledge the event
 * or mutate state, shadows, budgets, selected code or guest RAM. The producer
 * owns priority/APIC/PIC eligibility (including CR8), coalescing and arrival.
 * accepted is called exactly once after successful delivery; it cannot fail.
 * Its only effect is acknowledging that event in the producer's context.
 * State, shadows, budgets and context are mutually disjoint and accessible.
 * No callback runs when entry validation, selection or a budget stops execution.
 * 16=poll budget exhausted. Other statuses retain the continuation contract.
 * Successful event entry consumes no instruction budget and stops at the handler.
 * Shadows survive all rejected/unexecuted steps and expire on completion or
 * delivered exceptions. Incoming MOV-SS inhibition may be supplied explicitly;
 * unsupported MOV/POP SS still stop at their native boundary. */
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

uint64_t neverd_x64_continue_events_v6(
    neverd_x64_state_v6 *state, uint64_t *budget,
    neverd_x64_interruptibility_v1 *shadows, uint64_t *poll_budget,
    neverd_x64_peek_interrupt_v1 peek,
    neverd_x64_accept_interrupt_v1 accepted, void *context) {
    uint64_t last = 8;
    if (!*budget) return 9;
    if (!peek || !accepted || shadows->STIBlocked > 1 || shadows->SSBlocked > 1 ||
        shadows->NMIBlockedBySTI > 1 ||
        (shadows->STIBlocked && !(state->RFlags & 512))) return 5;
    for (unsigned i = 0; i < sizeof(shadows->Reserved); ++i)
        if (shadows->Reserved[i]) return 5;
    while (*budget) {
        uint64_t status;
        int sti;
        switch (state->PC) {
)C";
  for (const auto &Unit : Units)
    OS << "        case 0x" << llvm::utohexstr(Unit.Address)
       << "ULL: status = " << Unit.EntryValidation->Name
       << "((uint8_t *)state); sti = " << unsigned(Unit.IsSti) << "; break;\n";
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
            status = neverd_x64_deliver_event_v6(state, &event);
            if (status == 7) {
                shadows->STIBlocked = shadows->SSBlocked = 0;
                accepted(&pending, context);
                return status;
            }
            if (status != 15) return status;
        }
        const uint64_t old_flags = state->RFlags;
        switch (state->PC) {
)C";
  for (const auto &Unit : Units)
    OS << "        case 0x" << llvm::utohexstr(Unit.Address)
       << "ULL: status = " << Unit.Function.Name
       << "((uint8_t *)state); break;\n";
  OS << R"C(        default: return 5;
        }
        if (status == 4 || status == 5) return status;
        --*budget;
        shadows->STIBlocked = (status == 0 && sti && !(old_flags & 512));
        shadows->SSBlocked = 0;
)C";
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

} // namespace neverd
