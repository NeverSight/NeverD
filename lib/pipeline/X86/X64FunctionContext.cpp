//===- X64FunctionContext.cpp - Physical function activation ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep entry-return ownership separate from ordinary ABI inference.
//===----------------------------------------------------------------------===//

#include "X64FunctionContext.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>

namespace neverd {

X64FunctionContextNames x64FunctionContextNames(bool Debug) {
  if (Debug)
    return {"neverd_x64_function_context_v2",
            "neverd_x64_begin_debug_function_v6",
            "neverd_x64_begin_debug_function_at_v6",
            "neverd_x64_run_paged_debug_function_v6",
            "neverd_x64_continue_paged_debug_events_function_v6",
            "neverd_x64_call_context_v2",
            "neverd_x64_call_site_context_v2",
            "neverd_x64_begin_debug_call_site_handler_v1"};
  return {"neverd_x64_function_context_v1",
          "neverd_x64_begin_function_v6",
          {},
          "neverd_x64_run_paged_function_v6",
          "neverd_x64_continue_paged_events_function_v6",
          "neverd_x64_call_context_v1",
          "neverd_x64_call_site_context_v1",
          "neverd_x64_begin_call_site_handler_v1"};
}

llvm::StringRef x64FunctionCallPrefix(bool Paged, bool Scheduled, bool Debug) {
  if (Paged)
    return Debug ? (Scheduled ? "paged_debug_events_" : "paged_debug_")
                 : (Scheduled ? "paged_events_" : "paged_");
  return Scheduled ? "events_" : "";
}

void emitX64FunctionContext(llvm::raw_ostream &OS, unsigned Version,
                            llvm::ArrayRef<va_t> Entries, bool Debug) {
  const auto Names = x64FunctionContextNames(Debug);
  OS << R"C(
/* Caller-owned activation, disjoint from state, budgets and guest RAM.
 * Initialize with begin_function at a requested root; retain across pauses.
 * The original stack, including all incoming bytes, must already exist.
 * The producer declares this cell as the completion boundary; frame ownership
 * is not inferred. A selected near RET consuming that cell completes it.
 * Providers must not infer completion from the post-transfer PC or SP. */
enum neverd_x64_function_phase_v1 {
    NEVERD_X64_FUNCTION_UNINITIALIZED = 0,
    NEVERD_X64_FUNCTION_ACTIVE = 1,
    NEVERD_X64_FUNCTION_RETURNED = 2
};
)C";
  OS << "typedef struct " << Names.Context
     << " {\n    uint64_t ReturnSlot, Phase;\n";
  if (Debug)
    OS << "    uint64_t Status, Value; /* Original completion stop and RAX. "
          "*/\n";
  OS << "} " << Names.Context << ";\n";
  if (Debug) {
    OS << R"C(
/* A completed RET consumes the activation even if #DB enters a handler or
 * pauses its entry. Status/Value remain that original completion outcome;
 * later CPU event/handler execution is separate. Never replay the RET.
 * This new runtime symbol records state before post-instruction delivery. */
extern uint64_t neverd_x64_paged_transfer_v1(
    uint8_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *);
)C";
    OS << "static int " << Names.BeginAt << "(uint64_t pc, uint64_t sp, "
       << Names.Context
       << " *function) {\n"
          "    switch (pc) {\n";
  } else
    OS << "int neverd_x64_begin_function_v" << Version
       << "(const neverd_x64_state_v" << Version
       << " *state, neverd_x64_function_context_v1 *function) {\n"
          "    switch (state->PC) {\n";
  for (const auto Entry : Entries)
    OS << "    case 0x" << llvm::utohexstr(Entry) << "ULL:\n";
  OS << "        function->ReturnSlot = " << (Debug ? "sp" : "state->GPR[4]")
     << ";\n";
  if (Debug)
    OS << "        function->Status = function->Value = 0;\n";
  OS << "        function->Phase = NEVERD_X64_FUNCTION_ACTIVE;\n"
        "        return 1;\n"
        "    default: return 0;\n"
        "    }\n}\n";
  if (Debug)
    OS << "int " << Names.Begin << "(const neverd_x64_state_v" << Version
       << " *state, " << Names.Context << " *function) {\n"
       << "    return " << Names.BeginAt
       << "(state->PC, state->GPR[4], function);\n}\n";
}

void emitX64FunctionContextGuard(llvm::raw_ostream &OS, bool Debug) {
  using Exit = analysis::X64ContinuationExitV1;
  OS << "    if (function && function->Phase != NEVERD_X64_FUNCTION_ACTIVE)\n"
        "        return function->Phase == NEVERD_X64_FUNCTION_RETURNED ? ";
  if (Debug)
    OS << "function->Status";
  else
    OS << uint64_t(Exit::Return);
  OS << " : " << uint64_t(Exit::InvalidState) << ";\n";
}

void emitX64FunctionContextStep(
    llvm::raw_ostream &OS,
    const analysis::X64ContinuationInstructionSource &Unit,
    llvm::StringRef Indent, bool PagedMemory, llvm::StringRef PagedContext,
    llvm::function_ref<void(llvm::StringRef)> OnReturn,
    llvm::StringRef DebugSSShadow) {
  const bool Debug = !DebugSSShadow.empty();
  if (Unit.IsNearReturn)
    OS << Indent << "{\n"
       << Indent << "    uint64_t return_slot = state->GPR[4];\n";
  OS << Indent << "status = " << Unit.Function.Name;
  if (Debug)
    OS << "_paged_debug_step(state, " << PagedContext << ", " << DebugSSShadow
       << ");\n";
  else if (PagedMemory)
    OS << "_paged_step(state, " << PagedContext << ");\n";
  else
    OS << "((uint8_t *)state);\n";
  if (Unit.IsNearReturn) {
    if (Debug)
      OS << Indent << "    uint64_t completed_instruction = 0, value = 0;\n"
         << Indent << "    if (neverd_x64_paged_transfer_v1(" << PagedContext
         << ", &completed_instruction, 0, 0, &value) == "
         << uint64_t(analysis::X64ContinuationExitV1::Return)
         << " && completed_instruction == 0x" << llvm::utohexstr(Unit.Address)
         << "ULL";
    else
      OS << Indent << "    if (status == "
         << uint64_t(analysis::X64ContinuationExitV1::Return);
    OS << " && function && return_slot == function->ReturnSlot";
    if (PagedMemory && !Debug)
      OS << " &&\n"
         << Indent << "        (neverd_x64_paged_progress_v1(" << PagedContext
         << ") & " << analysis::kX64PagedInstructionAttempted << ")";
    OS << ") {\n" << Indent << "        --*budget;\n";
    if (OnReturn)
      OnReturn((Indent + "        ").str());
    if (Debug)
      OS << Indent << "        function->Status = status;\n"
         << Indent << "        function->Value = value;\n";
    OS << Indent << "        function->Phase = NEVERD_X64_FUNCTION_RETURNED;\n"
       << Indent << "        return status;\n"
       << Indent << "    }\n"
       << Indent << "}\n";
  }
}

void emitX64FunctionContextAdapter(llvm::raw_ostream &OS, unsigned Version) {
  OS << "uint64_t neverd_x64_continue_v" << Version << "(neverd_x64_state_v"
     << Version << " *state, uint64_t *budget) {\n"
     << "    return neverd_x64_continue_function_v" << Version
     << "(state, budget, 0);\n}\n";
}

} // namespace neverd
