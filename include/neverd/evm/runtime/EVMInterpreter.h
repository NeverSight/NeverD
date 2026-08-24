//===- EVMInterpreter.h - Deterministic EVM semantic oracle ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the deterministic, resource-bounded EVM interpreter used as the
/// semantic oracle for backend differential tests.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_RUNTIME_EVMINTERPRETER_H
#define NEVERD_EVM_RUNTIME_EVMINTERPRETER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::evm {

#define EVM_INTERPRETER_LIMIT(NAME, DEFAULT_VALUE)                             \
  inline constexpr size_t kDefault##NAME = (DEFAULT_VALUE);                    \
  inline constexpr llvm::StringLiteral k##NAME##Name = #NAME;
#include "neverd/evm/runtime/EVMInterpreterLimits.def"

struct WordLess {
  bool operator()(const llvm::APInt &Left, const llvm::APInt &Right) const {
    // APInt relational operators assert when widths differ. Ordering widths
    // first lets malformed public input reach validateEnvironment(), which
    // can return a diagnostic instead of aborting inside std::map insertion.
    if (Left.getBitWidth() != Right.getBitWidth())
      return Left.getBitWidth() < Right.getBitWidth();
    return Left.ult(Right);
  }
};

using WordMap = std::map<llvm::APInt, llvm::APInt, WordLess>;
using BytecodeMap = std::map<llvm::APInt, std::vector<uint8_t>, WordLess>;

/// Supplies deterministic values for EVM context and host-state operations.
struct ExecutionEnvironment {
  llvm::APInt Address = llvm::APInt(kWordBits, 0);
  llvm::APInt Origin = llvm::APInt(kWordBits, 0);
  llvm::APInt Caller = llvm::APInt(kWordBits, 0);
  llvm::APInt CallValue = llvm::APInt(kWordBits, 0);
  llvm::APInt GasPrice = llvm::APInt(kWordBits, 0);
  llvm::APInt Coinbase = llvm::APInt(kWordBits, 0);
  llvm::APInt Timestamp = llvm::APInt(kWordBits, 0);
  llvm::APInt BlockNumber = llvm::APInt(kWordBits, 0);
  uint64_t SlotNumber = 0;
  llvm::APInt Difficulty = llvm::APInt(kWordBits, 0);
  llvm::APInt PrevRandao = llvm::APInt(kWordBits, 0);
  llvm::APInt GasLimit = llvm::APInt(kWordBits, kDefaultGasLimit);
  llvm::APInt ChainID = llvm::APInt(kWordBits, kDefaultChainID);
  llvm::APInt BaseFee = llvm::APInt(kWordBits, 0);
  llvm::APInt BlobBaseFee = llvm::APInt(kWordBits, 0);
  llvm::APInt GasRemaining = llvm::APInt(kWordBits, kDefaultGasLimit);
  std::vector<uint8_t> Calldata;
  /// Optional pre-execution EIP-211 buffer snapshot. This is useful for
  /// resumed-state tests; it is not the current frame's final return value.
  std::vector<uint8_t> InitialReturnData;
  WordMap Storage;
  WordMap TransientStorage;
  WordMap Balances;
  WordMap CodeHashes;
  WordMap BlockHashes;
  BytecodeMap ExternalCode;
  std::vector<llvm::APInt> BlobHashes;
  /// Deterministic outcome exposed by CALL-family instructions.
  bool CallSuccess = true;
  std::vector<uint8_t> CallReturnData;
  /// Deterministic outcome exposed by CREATE/CREATE2. A failed creation pushes
  /// zero and may expose revert bytes through the EIP-211 return-data buffer.
  bool CreateSuccess = true;
  llvm::APInt CreatedAddress = llvm::APInt(kWordBits, 0);
  std::vector<uint8_t> CreateReturnData;
};

struct LogEntry {
  std::vector<llvm::APInt> Topics;
  std::vector<uint8_t> Data;
};

struct TraceEntry {
  uint64_t PC = 0;
  Opcode Op = Opcode::STOP;
  size_t StackBefore = 0;
  size_t StackAfter = 0;
};

enum class ExecutionStatus : uint8_t {
  Running,
  Stopped,
  Returned,
  Reverted,
  SelfDestructed,
  Faulted,
  /// Inconclusive resource cutoff; persistent state and logs are rolled back.
  StepLimit,
};

/// Classifies faulted execution without requiring a diagnostic allocation.
enum class ExecutionFaultKind : uint8_t {
  None,
  Semantic,
  ResourceExhausted,
};

/// Captures observable execution state, diagnostics, logs, and trace entries.
/// When HasPersistentStateSnapshot is true, reverted, faulted, and step-limited
/// results restore storage, transient storage, and logs to their entry snapshot
/// while retaining frame-local stack, memory, return bytes, and trace data for
/// diagnosis. REVERT therefore preserves its explicit return bytes without
/// exposing committable effects. Resource exhaustion is identified by
/// FaultKind even when allocating a human-readable Error is unsafe.
struct ExecutionResult {
  ExecutionStatus Status = ExecutionStatus::Running;
  ExecutionFaultKind FaultKind = ExecutionFaultKind::None;
  /// False only when resource exhaustion prevented the entry-state snapshot
  /// itself from being materialized. Such a result is never committable.
  bool HasPersistentStateSnapshot = true;
  std::string Error;
  uint64_t FinalPC = 0;
  size_t Steps = 0;
  std::vector<llvm::APInt> Stack;
  std::vector<uint8_t> Memory;
  WordMap Storage;
  WordMap TransientStorage;
  std::vector<uint8_t> ReturnData;
  std::vector<LogEntry> Logs;
  std::vector<TraceEntry> Trace;
  std::optional<llvm::APInt> SelfDestructBeneficiary;
};

/// Bounds execution resources and controls trace collection.
struct InterpreterOptions {
#define EVM_INTERPRETER_LIMIT(NAME, DEFAULT_VALUE) size_t NAME = kDefault##NAME;
#include "neverd/evm/runtime/EVMInterpreterLimits.def"
  bool RecordTrace = true;
};

/// Executes a decoded program. Runtime faults are returned in
/// `ExecutionResult::Status`; `llvm::Error` is reserved for API failures.
llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program);
llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program,
                                        ExecutionEnvironment &&Environment,
                                        InterpreterOptions Options = {});
llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program,
                                        const ExecutionEnvironment &Environment,
                                        InterpreterOptions Options = {});

} // namespace neverd::evm

#endif // NEVERD_EVM_RUNTIME_EVMINTERPRETER_H
