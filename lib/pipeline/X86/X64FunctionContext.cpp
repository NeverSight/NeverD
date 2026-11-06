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

void emitX64FunctionContext(llvm::raw_ostream &OS, unsigned Version,
                            llvm::ArrayRef<va_t> Entries) {
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
typedef struct neverd_x64_function_context_v1 {
    uint64_t ReturnSlot, Phase;
} neverd_x64_function_context_v1;
)C";
  OS << "int neverd_x64_begin_function_v" << Version
     << "(const neverd_x64_state_v" << Version
     << " *state, neverd_x64_function_context_v1 *function) {\n"
        "    switch (state->PC) {\n";
  for (const auto Entry : Entries)
    OS << "    case 0x" << llvm::utohexstr(Entry) << "ULL:\n";
  OS << "        function->ReturnSlot = state->GPR[4];\n"
        "        function->Phase = NEVERD_X64_FUNCTION_ACTIVE;\n"
        "        return 1;\n"
        "    default: return 0;\n"
        "    }\n}\n";
}

void emitX64FunctionContextGuard(llvm::raw_ostream &OS) {
  using Exit = analysis::X64ContinuationExitV1;
  OS << "    if (function && function->Phase != NEVERD_X64_FUNCTION_ACTIVE)\n"
        "        return function->Phase == NEVERD_X64_FUNCTION_RETURNED ? "
     << uint64_t(Exit::Return) << " : " << uint64_t(Exit::InvalidState)
     << ";\n";
}

void emitX64FunctionContextStep(
    llvm::raw_ostream &OS,
    const analysis::X64ContinuationInstructionSource &Unit,
    llvm::StringRef Indent, bool PagedMemory, llvm::StringRef PagedContext,
    llvm::function_ref<void(llvm::StringRef)> OnReturn) {
  if (Unit.IsNearReturn)
    OS << Indent << "{\n"
       << Indent << "    uint64_t return_slot = state->GPR[4];\n";
  OS << Indent << "status = " << Unit.Function.Name;
  if (PagedMemory)
    OS << "_paged_step(state, " << PagedContext << ");\n";
  else
    OS << "((uint8_t *)state);\n";
  if (Unit.IsNearReturn) {
    OS << Indent << "    if (status == "
       << uint64_t(analysis::X64ContinuationExitV1::Return)
       << " && function && return_slot == function->ReturnSlot";
    if (PagedMemory)
      OS << " &&\n"
         << Indent << "        (neverd_x64_paged_progress_v1(" << PagedContext
         << ") & " << analysis::kX64PagedInstructionAttempted << ")";
    OS << ") {\n" << Indent << "        --*budget;\n";
    if (OnReturn)
      OnReturn((Indent + "        ").str());
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
