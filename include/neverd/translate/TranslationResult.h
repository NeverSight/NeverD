//===- TranslationResult.h - Observable translation stop state -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONRESULT_H
#define NEVERD_TRANSLATE_TRANSLATIONRESULT_H

#include "neverd/translate/GuestState.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::translate {

/// Why host-native translation returned control to its caller.  Values are a
/// stable API and persistence contract; append new reasons without renumbering.
enum class TranslationStopReason : uint8_t {
  NotStarted = 0,
  Returned = 1,
  Syscall = 2,
  Exception = 3,
  Signal = 4,
  Breakpoint = 5,
  UnsupportedInstruction = 6,
  SelfModification = 7,
  BudgetExhausted = 8,
  Cancelled = 9,
  InternalError = 10,
  Dispatch = 11,
  ExternalCall = 12,
  MemoryFault = 13,
  JITUnavailable = 14,
};

static_assert(
    static_cast<uint8_t>(TranslationStopReason::NotStarted) == 0 &&
    static_cast<uint8_t>(TranslationStopReason::Returned) == 1 &&
    static_cast<uint8_t>(TranslationStopReason::Syscall) == 2 &&
    static_cast<uint8_t>(TranslationStopReason::Exception) == 3 &&
    static_cast<uint8_t>(TranslationStopReason::Signal) == 4 &&
    static_cast<uint8_t>(TranslationStopReason::Breakpoint) == 5 &&
    static_cast<uint8_t>(TranslationStopReason::UnsupportedInstruction) == 6 &&
    static_cast<uint8_t>(TranslationStopReason::SelfModification) == 7 &&
    static_cast<uint8_t>(TranslationStopReason::BudgetExhausted) == 8 &&
    static_cast<uint8_t>(TranslationStopReason::Cancelled) == 9 &&
    static_cast<uint8_t>(TranslationStopReason::InternalError) == 10 &&
    static_cast<uint8_t>(TranslationStopReason::Dispatch) == 11 &&
    static_cast<uint8_t>(TranslationStopReason::ExternalCall) == 12 &&
    static_cast<uint8_t>(TranslationStopReason::MemoryFault) == 13 &&
    static_cast<uint8_t>(TranslationStopReason::JITUnavailable) == 14);

const char *translationStopReasonName(TranslationStopReason Reason);
bool isResumableTranslationStop(TranslationStopReason Reason);

enum class MemoryAccessKind : uint8_t {
  Read = 1,
  Write = 2,
  Execute = 3,
  AtomicReadModifyWrite = 4,
};

enum class TranslationTrapKind : uint8_t {
  Breakpoint = 1,
  UnsupportedInstruction = 2,
  Exception = 3,
  Signal = 4,
};

struct MemoryFaultExit {
  uint64_t Address = 0;
  MemoryAccessKind Access = MemoryAccessKind::Read;
  uint32_t AccessWidthBits = 0;
  /// Required alignment in bytes.  Zero means the guest operation imposed no
  /// additional alignment.
  uint32_t RequiredAlignment = 0;
};

struct SelfModificationExit {
  uint64_t Address = 0;
  uint64_t Size = 0;
  uint64_t OldGeneration = 0;
  uint64_t NewGeneration = 0;
};

enum class TranslationBudgetKind : uint8_t {
  GuestInstructions = 1,
  Blocks = 2,
  GeneratedCodeBytes = 3,
};

struct BudgetExit {
  TranslationBudgetKind Kind = TranslationBudgetKind::GuestInstructions;
  uint64_t Limit = 0;
  uint64_t Observed = 0;
};

struct SyscallExit {
  uint64_t Number = 0;
  std::vector<uint64_t> Arguments;
};

struct ExternalCallExit {
  uint64_t TargetAddress = 0;
  std::string Symbol;
  std::vector<uint64_t> Arguments;
};

struct TrapExit {
  TranslationTrapKind Kind = TranslationTrapKind::UnsupportedInstruction;
  uint64_t Code = 0;
  uint64_t Subcode = 0;
  uint64_t Address = 0;
  bool Restartable = false;
};

/// Versioned, typed exit payload.  Optional records are mutually exclusive and
/// validateTranslationResult() requires the record selected by Reason.  This
/// avoids a reason-dependent integer whose meaning changes across consumers.
inline constexpr uint32_t kTranslationExitVersion = 1;
struct TranslationExit {
  uint32_t Version = kTranslationExitVersion;
  TranslationStopReason Reason = TranslationStopReason::NotStarted;
  uint64_t PC = 0;
  uint64_t NextPC = 0;
  std::optional<MemoryFaultExit> MemoryFault;
  std::optional<SelfModificationExit> SelfModification;
  std::optional<BudgetExit> Budget;
  std::optional<SyscallExit> Syscall;
  std::optional<ExternalCallExit> ExternalCall;
  std::optional<TrapExit> Trap;
  bool FallbackRequested = false;
  std::string Diagnostic;
};

struct TranslationResult {
  GuestArchitecture Guest = GuestArchitecture::X86_64;
  GuestArchitecture Host = GuestArchitecture::AArch64;
  uint64_t StartPC = 0;
  uint64_t GuestInstructions = 0;
  uint64_t BlocksTranslated = 0;
  uint64_t GeneratedCodeBytes = 0;
  TranslationExit Exit;
};

struct TranslationOptions;

/// Validate the self-contained shape and internal counters of a result.
llvm::Error validateTranslationResult(const TranslationResult &Result);

/// Validate a result against the exact request that governed execution.  This
/// binds budget limits, fallback permission, guest identity, and explicit AOT
/// host identity instead of trusting result-owned policy fields.
llvm::Error validateTranslationResult(const TranslationResult &Result,
                                      const TranslationOptions &Options);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONRESULT_H
