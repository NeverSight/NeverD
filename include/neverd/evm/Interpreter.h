//===- Interpreter.h - Deterministic EVM semantic oracle ------*- C++ -*-===//

#ifndef NEVERD_EVM_INTERPRETER_H
#define NEVERD_EVM_INTERPRETER_H

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::evm {

struct WordLess {
  bool operator()(const llvm::APInt &Left, const llvm::APInt &Right) const {
    return Left.ult(Right);
  }
};

using WordMap = std::map<llvm::APInt, llvm::APInt, WordLess>;
using BytecodeMap = std::map<llvm::APInt, std::vector<uint8_t>, WordLess>;

struct ExecutionEnvironment {
  llvm::APInt Address = llvm::APInt(kWordBits, 0);
  llvm::APInt Origin = llvm::APInt(kWordBits, 0);
  llvm::APInt Caller = llvm::APInt(kWordBits, 0);
  llvm::APInt CallValue = llvm::APInt(kWordBits, 0);
  llvm::APInt GasPrice = llvm::APInt(kWordBits, 0);
  llvm::APInt Coinbase = llvm::APInt(kWordBits, 0);
  llvm::APInt Timestamp = llvm::APInt(kWordBits, 0);
  llvm::APInt BlockNumber = llvm::APInt(kWordBits, 0);
  llvm::APInt PrevRandao = llvm::APInt(kWordBits, 0);
  llvm::APInt GasLimit = llvm::APInt(kWordBits, kDefaultGasLimit);
  llvm::APInt ChainID = llvm::APInt(kWordBits, kDefaultChainID);
  llvm::APInt BaseFee = llvm::APInt(kWordBits, 0);
  llvm::APInt BlobBaseFee = llvm::APInt(kWordBits, 0);
  llvm::APInt GasRemaining = llvm::APInt(kWordBits, kDefaultGasLimit);
  std::vector<uint8_t> Calldata;
  std::vector<uint8_t> InitialReturnData;
  WordMap Storage;
  WordMap TransientStorage;
  WordMap Balances;
  WordMap CodeHashes;
  WordMap BlockHashes;
  BytecodeMap ExternalCode;
  std::vector<llvm::APInt> BlobHashes;
  bool CallSuccess = true;
  std::vector<uint8_t> CallReturnData;
  llvm::APInt CreatedAddress = llvm::APInt(kWordBits, 0);
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
  StepLimit,
};

struct ExecutionResult {
  ExecutionStatus Status = ExecutionStatus::Running;
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

struct InterpreterOptions {
  size_t MaxSteps = kDefaultMaxSteps;
  size_t MaxMemoryBytes = kDefaultMaxMemoryBytes;
  bool RecordTrace = true;
};

llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program,
                                        ExecutionEnvironment Environment = {},
                                        InterpreterOptions Options = {});

} // namespace neverd::evm

#endif // NEVERD_EVM_INTERPRETER_H
