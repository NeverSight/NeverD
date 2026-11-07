//===- X64ContinuationPagedDriver.h - Paged region driver ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit bounded dispatch through the shared paged instruction executor.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_PIPELINE_X86_X64CONTINUATIONPAGEDDRIVER_H
#define NEVERD_PIPELINE_X86_X64CONTINUATIONPAGEDDRIVER_H

#include "X64FunctionCallSites.h"
#include "X64FunctionContext.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {
inline void emitX64PagedDriver(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    const analysis::X64ContinuationSourceOptions &Options, bool FollowCalls,
    bool FunctionContext, const X64FunctionCallSitePauses &CallSitePauses) {
  const auto Names = x64FunctionContextNames(Options.PagedDebug);
  OS << R"C(
/* Link the NeverD paged execution bridge. Context points to X64PagedExecution.
 * Each instruction verifies its original bytes through guest fetch permissions,
 * commits scalar state/data on success, or delivers its fault before returning.
 * 18=backing failure, 19=shutdown, 20=changed code requiring relift.
 * Architectural faults consume one attempt, including failed fault entry.
 * Refused steps, changed code and scalar operand backing stops consume none.
 * An authenticated IRET that begins frame access consumes an attempt even if
 * backing fails: NMI unblocking and earlier descriptor writes are retained.
 * MOV SS requires the scheduled entry, whose shadow persists across calls. */
typedef uint64_t (*neverd_x64_paged_body_v1)(void *, void *);
extern uint64_t neverd_x64_paged_step_v1(
    uint8_t *, uint8_t *, uint64_t, const uint8_t *, uint64_t,
    neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t);
extern uint64_t neverd_x64_paged_validate_v1(
    uint8_t *, uint8_t *, uint64_t, neverd_x64_paged_body_v1, uint64_t);
extern uint64_t neverd_x64_paged_event_v1(
    uint8_t *, uint8_t *, const uint8_t *, uint64_t);
extern uint64_t neverd_x64_paged_progress_v1(uint8_t *);
)C";
  if (Options.PagedDebug)
    OS << R"C(
extern uint64_t neverd_x64_paged_debug_step_v2(
    uint8_t *, uint8_t *, uint64_t, const uint8_t *, uint64_t,
    neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t,
    uint64_t);
extern uint64_t neverd_x64_paged_debug_validate_v2(
    uint8_t *, uint8_t *, uint64_t, neverd_x64_paged_body_v1, uint64_t);
)C";
  for (const auto &Unit : Units)
    for (const auto *Name : {&Unit.Function.Name, &Unit.EntryValidation->Name})
      OS << "static uint64_t " << *Name
         << "_paged_adapter(void *state, void *exchange) {\n"
         << "    return " << *Name << "(state, exchange);\n}\n";
  for (const auto &Unit : Units) {
    OS << "static uint64_t " << Unit.Function.Name
       << (Options.PagedDebug ? "_paged_debug_step" : "_paged_step")
       << "(neverd_x64_state_v6 *state, uint8_t *context"
       << (Options.PagedDebug ? ", uint64_t ss_blocked" : "") << ") {\n"
       << "    static const uint8_t bytes[] = {";
    for (auto Byte : Unit.Bytes)
      OS << unsigned(Byte) << ',';
    OS << "};\n    return "
       << (Options.PagedDebug ? "neverd_x64_paged_debug_step_v2"
                              : "neverd_x64_paged_step_v1")
       << "(context, (uint8_t *)state, "
          "0x"
       << llvm::utohexstr(Unit.Address) << "ULL, bytes, sizeof(bytes), "
       << Unit.Function.Name << "_paged_adapter, " << Unit.EntryValidation->Name
       << "_paged_adapter, " << *Options.MXCSRMask << ", "
       << *Options.PagingAddressBits
       << (Options.PagedDebug ? ", ss_blocked" : "") << ");\n}\n";
    if (Options.PagedDebug)
      OS << "static uint64_t " << Unit.Function.Name
         << "_paged_step(neverd_x64_state_v6 *state, uint8_t *context) {\n"
         << "    return " << Unit.Function.Name
         << "_paged_debug_step(state, context, 0);\n}\n";
  }
  OS << "\nuint64_t "
     << (FunctionContext ? Names.PagedDriver : "neverd_x64_run_paged_v6")
     << "(neverd_x64_state_v6 *state, uint8_t *context,\n"
        "                                uint64_t *budget";
  if (FunctionContext)
    OS << ", " << Names.Context << " *function";
  OS << ") {\n";
  if (FunctionContext)
    emitX64FunctionContextGuard(OS, Options.PagedDebug);
  OS << R"C(    uint64_t last = 8;
    while (*budget) {
        uint64_t status;
        switch (state->PC) {
)C";
  for (const auto &Unit : Units) {
    OS << "        case 0x" << llvm::utohexstr(Unit.Address) << "ULL: ";
    if (CallSitePauses.emit(OS, Unit.Address, ""))
      continue;
    if (Unit.IsMovSS && !Unit.RequiresNativeExecution)
      OS << "return 5; /* Persistent MOV-SS inhibition needs the scheduler. "
            "*/\n";
    else if (FunctionContext && Unit.IsNearReturn) {
      OS << "\n";
      emitX64FunctionContextStep(OS, Unit, "            ", true, "context", {},
                                 Options.PagedDebug ? "0" : "");
      OS << "            break;\n";
    } else
      OS << "status = " << Unit.Function.Name
         << "_paged_step(state, context); break;\n";
  }
  OS << R"C(
        default: return last == 2 ? 2 : 8;
        }
)C";
  OS << "        if (!(neverd_x64_paged_progress_v1(context) & "
     << analysis::kX64PagedInstructionAttempted << ")) return status;\n"
     << "        --*budget;\n";
  OS << "        if (status == "
     << uint64_t(analysis::X64PagedExit::RepeatContinue) << ") status = 0;\n";
  if (FollowCalls) {
    OS << "        if (status == 1 || status == 3) {\n"
          "            switch (state->PC) {\n";
    for (const auto &Unit : Units)
      OS << "            case 0x" << llvm::utohexstr(Unit.Address) << "ULL:\n";
    OS << "                break;\n            default: return status;\n"
          "            }\n            last = status;\n            continue;\n"
          "        }\n";
  }
  OS << "        if (status != 0 && status != 2) return status;\n"
        "        last = status;\n    }\n    return 9;\n}\n";
  if (FunctionContext)
    OS << R"C(
uint64_t neverd_x64_run_paged_v6(neverd_x64_state_v6 *state, uint8_t *context,
                                uint64_t *budget) {
    return )C"
       << Names.PagedDriver << R"C((state, context, budget, 0);
}
)C";
}
} // namespace neverd
#endif // NEVERD_PIPELINE_X86_X64CONTINUATIONPAGEDDRIVER_H
