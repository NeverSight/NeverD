//===- X86ModeExecution.h - Continuous x86 execution modes ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One full-width bank across protected, compatibility and 64-bit execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X86MODEEXECUTION_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X86MODEEXECUTION_H

#include "neverd/analysis/arch/x86_64/X64PagedExecution.h"
#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::analysis {

enum class X86ExecutionMode : uint8_t {
  Protected16,
  Protected32,
  Compatibility16,
  Compatibility32,
  Long64,
};

namespace detail {
struct X86ModeContinuation;
}

struct X86ModeState {
  X64ContinuationStateV7 Core;
  std::optional<X64PagedDebugState> Debug;
  bool STIBlocked = false;
  bool SSBlocked = false;
  bool Halted = false;
  bool Shutdown = false;
  /// Private instruction/event progress and frozen execution bindings. Keep
  /// this with CPU/RAM while repairing backing or rebuilding the executor.
  std::shared_ptr<const detail::X86ModeContinuation> Pending;
};

/// GPR0..7 upper-half choices use the existing V1 Undefined slots. Crossing
/// LMA=0 additionally loses the architectural values of R8..15/XMM8..15.
/// This binding supplies those choices before either bank is observed again.
/// Pure long/compatibility transitions preserve the extended registers.
struct X86ModeUndefinedState {
  std::array<uint64_t, 8> GPR{};
  std::array<std::array<uint8_t, 16>, 8> XMM{};
};

/// All undefined choices, including Legacy.Segments.FullLimitChoice, must be
/// bound before the first instruction. A suspended instruction freezes them.
struct X86ModeExecutionOptions {
  translate::X86LegacyExecutionOptions Legacy;
  X64PagedExecutionOptions Long;
  uint32_t MXCSRMask = 0;
  unsigned PagingAddressBits = 0;
  std::optional<X86ModeUndefinedState> Undefined;
  bool EFERSystemCall = false;
};

/// The full address and mode jointly identify an original instruction.
/// Body/Validate come from the same authenticated paged source export, only
/// for Long64, through wrappers with the exact X64PagedBody function type.
/// Keep their libraries and borrowed callback contexts alive through resume.
/// Non-64 instructions use the shared original-byte executor.
struct X86ModeInstruction {
  uint64_t Address = 0;
  X86ExecutionMode Mode = X86ExecutionMode::Protected32;
  std::vector<uint8_t> Bytes;
  X64PagedBody Body = nullptr;
  X64PagedBody Validate = nullptr;
  bool BranchDebug = false;
};

struct X86ModeExecutionResult : translate::X86LegacyExecutionResult {
  bool BudgetExhausted = false;
  bool MissingInstruction = false;
  /// A post-instruction owner refusal retains its pending event and reports
  /// the instruction receipt instead of losing it in a bare Error.
  std::string OwnerError;
  uint64_t Attempts = 0;
  uint64_t Completions = 0;
};

/// Confined ordinary RAM and the explicitly bound Intel profiles of the two
/// underlying owners. No host control/segment rewrites occur between modes.
/// Existing V6/V7 wire layouts are unchanged. Pending work retains the original
/// bindings; constructing an executor with different options affects only the
/// next instruction, never an already accepted instruction or event.
class X86ModeExecution final {
  translate::X86PagedMemory &Memory;
  X86ModeExecutionOptions Options;

public:
  X86ModeExecution(translate::X86PagedMemory &Memory,
                   const X86ModeExecutionOptions &Options)
      : Memory(Memory), Options(Options) {}

  llvm::Expected<X86ExecutionMode> mode(const X86ModeState &State) const;
  llvm::Expected<X86ModeExecutionResult>
  run(X86ModeState &State, llvm::ArrayRef<X86ModeInstruction> Instructions,
      uint64_t &Budget);

  /// Supply an externally selected IRQ or NMI at the current instruction
  /// boundary. Arrival/priority/APIC policy belongs to the producer. The CPU
  /// owns saved PC and inhibition. A deferred/refused input has no effects;
  /// acceptance is reported exactly once, including across entry suspension.
  /// Resume pending work with run() before supplying another input. Shutdown
  /// is terminal until the embedding supplies an explicitly reset machine.
  llvm::Expected<X86ModeExecutionResult>
  interrupt(X86ModeState &State, X86EventKind Kind, uint8_t Vector);

private:
  llvm::Expected<X86ModeExecutionResult>
  step(X86ModeState &State, const X86ModeInstruction *Instruction);
};

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X86MODEEXECUTION_H
