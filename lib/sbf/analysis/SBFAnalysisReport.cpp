//===- SBFAnalysisReport.cpp - SBF stage and coverage evidence ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Summarizes the canonical SBF image and IR without reinterpreting semantics
/// or constructing a second function-discovery or control-flow analysis.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFAnalysisReport.h"

#include "neverd/sbf/SBFIR.h"

namespace neverd::sbf {

llvm::StringRef analysisStageName(AnalysisStage Stage) {
  switch (Stage) {
#define SBF_ANALYSIS_STAGE(ID, NAME)                                           \
  case AnalysisStage::ID:                                                      \
    return NAME;
#include "neverd/sbf/analysis/SBFAnalysisStages.def"
  }
  return "unknown";
}

std::optional<AnalysisStage> parseAnalysisStage(llvm::StringRef Name) {
#define SBF_ANALYSIS_STAGE(ID, NAME)                                           \
  if (Name == NAME)                                                            \
    return AnalysisStage::ID;
#include "neverd/sbf/analysis/SBFAnalysisStages.def"
  return std::nullopt;
}

llvm::StringRef analysisStageStateName(AnalysisStageState State) {
  switch (State) {
  case AnalysisStageState::Started:
    return "started";
  case AnalysisStageState::Passed:
    return "passed";
  case AnalysisStageState::Failed:
    return "failed";
  }
  return "unknown";
}

llvm::json::Object analysisCoverage(const SBFProgram &Program) {
  uint64_t Continuations = 0, Invalid = 0, Internal = 0, Syscalls = 0;
  uint64_t Indirect = 0, Reachable = 0, Owned = 0, Ambiguous = 0;
  uint64_t UnknownSyscalls = 0, UnsupportedCalls = 0, UnresolvedCalls = 0;
  for (const LowInstruction &Instruction : Program.Low.Instructions) {
    Continuations += Instruction.IsContinuation;
    Invalid += Instruction.isInvalid();
    Internal += Instruction.Call == CallKind::Internal;
    Syscalls += Instruction.Call == CallKind::Syscall;
    Indirect += Instruction.Call == CallKind::Indirect;
    UnsupportedCalls += Instruction.Call == CallKind::Unsupported;
    UnresolvedCalls += Instruction.Call == CallKind::Unresolved;
    UnknownSyscalls += Instruction.Call == CallKind::Syscall &&
                       !Program.isSyscallRegistered(Instruction.SyscallHash);
  }
  for (const BasicBlock &Block : Program.Low.Blocks) {
    const uint64_t Slots = Block.EndSlot - Block.StartSlot;
    if (Block.Reachable)
      Reachable += Slots;
    if (Block.ID < Program.High.BlockOwners.size()) {
      const size_t Owner = Program.High.BlockOwners[Block.ID];
      if (Owner < Program.High.Functions.size())
        Owned += Slots;
      else if (Owner == HighIR::AmbiguousFunction)
        Ambiguous += Slots;
    }
  }
  llvm::json::Object Counts;
  Counts["text_bytes"] = static_cast<int64_t>(Program.text().size());
  Counts["expected_slots"] =
      static_cast<int64_t>(Program.text().size() / kInstructionSize);
  Counts["decoded_slots"] =
      static_cast<int64_t>(Program.Low.Instructions.size());
  Counts["logical_instructions"] =
      static_cast<int64_t>(Program.Low.Instructions.size() - Continuations);
  Counts["continuation_slots"] = static_cast<int64_t>(Continuations);
  Counts["invalid_slots"] = static_cast<int64_t>(Invalid);
  Counts["blocks"] = static_cast<int64_t>(Program.Low.Blocks.size());
  Counts["edges"] = static_cast<int64_t>(Program.Low.Edges.size());
  Counts["reachable_slots"] = static_cast<int64_t>(Reachable);
  Counts["function_owned_slots"] = static_cast<int64_t>(Owned);
  Counts["ambiguous_function_slots"] = static_cast<int64_t>(Ambiguous);
  Counts["functions"] = static_cast<int64_t>(Program.High.Functions.size());
  Counts["med_instructions"] =
      static_cast<int64_t>(Program.Med.Instructions.size());
  Counts["regions"] = static_cast<int64_t>(Program.High.Regions.size());
  Counts["internal_calls"] = static_cast<int64_t>(Internal);
  Counts["syscalls"] = static_cast<int64_t>(Syscalls);
  Counts["unregistered_syscalls"] = static_cast<int64_t>(UnknownSyscalls);
  Counts["unresolved_indirect_calls"] = static_cast<int64_t>(Indirect);
  Counts["unsupported_calls"] = static_cast<int64_t>(UnsupportedCalls);
  Counts["unresolved_calls"] = static_cast<int64_t>(UnresolvedCalls);
  Counts["diagnostics"] = static_cast<int64_t>(Program.Low.Diagnostics.size());
  Counts["handlers"] =
      static_cast<int64_t>(Program.High.Solana.Handlers.size());
  Counts["account_accesses"] =
      static_cast<int64_t>(Program.High.Solana.AccountAccesses.size());
  Counts["cpi_sites"] =
      static_cast<int64_t>(Program.High.Solana.CPISites.size());
  return Counts;
}

} // namespace neverd::sbf
