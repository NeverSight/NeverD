//===- X64ReturnStackEvents.h - Paged instruction exits --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit C entry points into the current guest state, including switched stacks.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64RETURNSTACKEVENTS_H
#define NEVERD_LIB_PIPELINE_X86_X64RETURNSTACKEVENTS_H

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <utility>

namespace neverd {
inline constexpr char kX64ReturnStackEvents[] = "return_stack_events";
inline constexpr char kX64ReturnStackResume[] =
    "neverd_x64_resume_paged_transfers_v6";

inline llvm::json::Object
emitX64ReturnStackEntries(llvm::raw_ostream &OS, llvm::ArrayRef<va_t> Roots,
                          const std::set<va_t> &JumpStops) {
  llvm::json::Array Entries;
  for (va_t Root : Roots) {
    const std::string Address = llvm::utohexstr(Root);
    const std::string Name = "neverd_x64_enter_paged_" + Address + "_v6";
    OS << "\nuint64_t " << Name
       << "(neverd_x64_state_v6 *state, uint8_t *context, uint64_t *budget) {\n"
       << "    if (!state || state->PC != 0x" << Address << "ULL) return "
       << uint64_t(analysis::X64ContinuationExitV1::InvalidState) << ";\n"
       << "    return " << kX64ReturnStackResume
       << "(state, context, budget);\n}\n";
    Entries.push_back(
        llvm::json::Object{{"entry", "0x" + Address}, {"symbol", Name}});
  }
  llvm::json::Array Jumps;
  for (va_t PC : JumpStops)
    Jumps.push_back("0x" + llvm::utohexstr(PC));
  return llvm::json::Object{
      {"entries", std::move(Entries)},
      {"resume", kX64ReturnStackResume},
      {"jump_stops", std::move(Jumps)},
      {"stack", "instruction-time guest registers and paged RAM"},
      {"completion", "stop after physical CALL, RET, or a listed jump; "
                     "return the instruction exit and updated guest state"},
      {"context", "retain the same offline X64PagedExecution across stops"}};
}
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64RETURNSTACKEVENTS_H
